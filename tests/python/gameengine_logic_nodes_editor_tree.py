# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import math
import re
import sys
from pathlib import Path

import addon_utils
import bl_ui
import bpy
from bl_ui import node_add_menu, space_node

LOGIC_NODES_TREE_PROPERTY = "__ln_logic_tree"
LOGIC_NODES_ENABLED_PROPERTY = "__ln_logic_enabled"
LOGIC_NODES_STATUS_PROPERTY = "__ln_logic_status"
LOGIC_NODES_BINDING_OBJECT = "LN Binding Probe"
LOGIC_NODES_CLEAR_OBJECT = "LN Binding Clear Probe"
EXPECTED_LINKED_NODE_COUNT = None
EXPECTED_LINKED_LINK_COUNT = None

VALUE_OUTPUT_KEYS = ("Bool", "Int", "Float", "String", "Vector", "Value")


def value_output(node):
    for key in VALUE_OUTPUT_KEYS:
        if key in node.outputs:
            return node.outputs[key]
    raise KeyError(f"{node.bl_idname} has no value output socket")


def vector_input(node, *names):
    for key in names:
        if key in node.inputs:
            return node.inputs[key]
    raise KeyError(f"{node.bl_idname} missing vector input {names}")


def new_get_object_attribute(tree, attribute_type):
    node = tree.nodes.new("LogicNativeGetObjectAttribute")
    node.attribute_type = attribute_type
    return node


def new_set_object_attribute(tree, attribute_type):
    node = tree.nodes.new("LogicNativeSetObjectAttribute")
    node.attribute_type = attribute_type
    return node


def flow_input(node):
    if "Flow" in node.inputs:
        return node.inputs["Flow"]
    raise KeyError(f"{node.bl_idname} has no Flow input socket")


LOGIC_SOCKET_COLORS = {
    "NodeSocketLogicExecution": (0.9, 0.3, 0.3, 1.0),
    "NodeSocketLogicCondition": (0.8, 0.651, 0.839, 1.0),
    "NodeSocketLogicGeneric": (0.631, 0.631, 0.631, 1.0),
    "NodeSocketLogicList": (0.341, 0.714, 0.620, 1.0),
    "NodeSocketLogicDictionary": (0.957, 0.667, 0.282, 1.0),
    "NodeSocketLogicBool": (0.8, 0.651, 0.839, 1.0),
    "NodeSocketLogicInt": (0.349, 0.549, 0.361, 1.0),
    "NodeSocketLogicCollisionLayers": (0.349, 0.549, 0.361, 1.0),
    "NodeSocketLogicFloat": (0.631, 0.631, 0.631, 1.0),
    "NodeSocketFloatAngle": (0.63, 0.63, 0.63, 1.0),
    "NodeSocketLogicString": (0.439, 0.698, 1.0, 1.0),
    "NodeSocketLogicVector": (0.388, 0.388, 0.78, 1.0),
    "NodeSocketLogicRotation": (0.651, 0.388, 0.78, 1.0),
    "NodeSocketLogicColor": (0.78, 0.78, 0.161, 1.0),
    "NodeSocketLogicObject": (0.929, 0.620, 0.361, 1.0),
    "NodeSocketLogicScene": (0.349, 0.769, 0.894, 1.0),
    "NodeSocketLogicCollection": (0.443, 0.733, 0.361, 1.0),
    "NodeSocketLogicMaterial": (0.827, 0.518, 0.337, 1.0),
    "NodeSocketLogicImage": (0.341, 0.545, 0.882, 1.0),
    "NodeSocketLogicSound": (0.859, 0.612, 0.220, 1.0),
    "NodeSocketLogicFont": (0.718, 0.455, 0.804, 1.0),
    "NodeSocketLogicGeometryTree": (0.396, 0.659, 0.529, 1.0),
    "NodeSocketLogicText": (0.478, 0.678, 0.737, 1.0),
    "NodeSocketLogicMesh": (0.565, 0.686, 0.459, 1.0),
    "NodeSocketLogicDatablock": (0.620, 0.569, 0.498, 1.0),
    "NodeSocketLogicUI": (0.471, 0.596, 0.784, 1.0),
}

LOGIC_CONDITION_SOCKET_IDNAME = "NodeSocketLogicCondition"
LOGIC_EXECUTION_SOCKET_IDNAME = "NodeSocketLogicExecution"

UNSUPPORTED_COMMAND_HANDLER_RE = re.compile(
    r"COMMAND_HANDLER\(\s*([A-Za-z0-9_]+)\s*,\s*\"[^\"]+\"\s*,\s*"
    r"CompileUnsupportedCommandNode\s*\)"
)

EXPECTED_LOGIC_MATH_OPERATIONS = {
    "ADD",
    "SUBTRACT",
    "MULTIPLY",
    "DIVIDE",
    "POWER",
    "MINIMUM",
    "MAXIMUM",
    "ABSOLUTE",
    "SIGN",
    "ROUND",
    "FLOOR",
    "CEIL",
    "TRUNC",
    "FRACT",
    "MODULO",
    "SINE",
    "COSINE",
    "RADIANS",
    "DEGREES",
}

EXPECTED_LOGIC_VECTOR_MATH_OPERATIONS = {
    "ADD",
    "SUBTRACT",
    "MULTIPLY",
    "DIVIDE",
    "SCALE",
    "NORMALIZE",
    "ABSOLUTE",
    "MINIMUM",
    "MAXIMUM",
}

EXPECTED_LOGIC_STRING_OPERATION_OPERATIONS = {
    "JOIN",
    "CONTAINS",
    "COUNT",
    "REPLACE",
    "STARTS_WITH",
    "ENDS_WITH",
    "UPPER",
    "LOWER",
    "ZFILL",
}

EXPECTED_LOGIC_VSYNC_MODES = {"OFF", "ON", "ADAPTIVE"}
EXPECTED_LOGIC_VEHICLE_AXES = {"REAR", "FRONT", "ALL"}
EXPECTED_LOGIC_CONSTRAINT_BREAK_MATCH_MODES = {"EXACT", "CONTAINS"}

EXPECTED_SOCKET_IDNAMES = {
    "LogicNativeOnInit": {
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeOnGameLoaded": {
        "outputs": {
            "Out": "NodeSocketLogicExecution",
            "Success": "NodeSocketLogicBool",
        },
    },
    "LogicNativeOnUpdate": {
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeWhilePaused": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Collection": "NodeSocketLogicCollection",
        },
        "outputs": {
            "Update": "NodeSocketLogicExecution",
            "Delta Time": "NodeSocketLogicFloat",
        },
        "rna_enums": {"pause_scope": {"OWNER", "OBJECT_HIERARCHY", "COLLECTION", "SCENE"}},
    },
    "LogicNativeOnFixedUpdate": {
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeOnConstraintBreak": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Constraint Name": "NodeSocketLogicString",
            "Match Any": "NodeSocketLogicBool",
        },
        "outputs": {
            "Broken": "NodeSocketLogicExecution",
            "Constraint Name": "NodeSocketLogicString",
            "Target Object": "NodeSocketLogicObject",
            "Anchor Position": "NodeSocketLogicVector",
            "Linear Impulse": "NodeSocketLogicFloat",
            "Angular Impulse": "NodeSocketLogicFloat",
            "Applied Impulse": "NodeSocketLogicFloat",
            "Break Count": "NodeSocketLogicInt",
            "Break Events": "NodeSocketLogicList",
        },
    },
    "LogicNativePlayAction": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Start Frame": "NodeSocketLogicFloat",
            "End Frame": "NodeSocketLogicFloat",
            "Layer": "NodeSocketLogicInt",
            "Priority": "NodeSocketLogicInt",
            "Blend In": "NodeSocketLogicFloat",
            "Speed": "NodeSocketLogicFloat",
        },
        "outputs": {
            "Started": "NodeSocketLogicExecution",
            "Completed": "NodeSocketLogicExecution",
            "Interrupted": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
        },
        "rna_enums": {
            "play_mode": {"PLAY", "LOOP", "PING_PONG"},
            "blend_mode": {"BLEND", "ADD"},
        },
    },
    "LogicNativeOnNextFrame": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
        "outputs": {"Completed": "NodeSocketLogicExecution"},
    },
    "LogicNativeBranch": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Condition": "NodeSocketLogicCondition",
        },
        "outputs": {
            "True": "NodeSocketLogicExecution",
            "False": "NodeSocketLogicExecution",
        },
    },
    "LogicNativeFlowGate": {
        "inputs": {
            "Enter": "NodeSocketLogicExecution",
            "Toggle": "NodeSocketLogicExecution",
            "Open": "NodeSocketLogicExecution",
            "Close": "NodeSocketLogicExecution",
            "Start Open": "NodeSocketLogicBool",
        },
        "outputs": {
            "Exit": "NodeSocketLogicExecution",
            "Is Open": "NodeSocketLogicBool",
        },
    },
    "LogicNativeAnyFlow": {
        "inputs": {"In": "NodeSocketLogicExecution"},
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSequence": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
        "outputs": {
            "Then 0": "NodeSocketLogicExecution",
            "Then 1": "NodeSocketLogicExecution",
        },
    },
    "LogicNativeGate": {
        "inputs": {"Conditions": "NodeSocketLogicCondition"},
        "outputs": {"Result": "NodeSocketLogicCondition"},
    },
    "LogicNativeOnce": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Reset": "NodeSocketLogicExecution",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeBooleanEdge": {
        "inputs": {"Condition": "NodeSocketLogicCondition"},
        "outputs": {
            "Rising": "NodeSocketLogicExecution",
            "Falling": "NodeSocketLogicExecution",
        },
    },
    "LogicNativeValueChanged": {
        "inputs": {"Value": "NodeSocketLogicGeneric"},
        "outputs": {
            "If Changed": "NodeSocketLogicExecution",
            "Old": "NodeSocketLogicGeneric",
            "New": "NodeSocketLogicGeneric",
        },
    },
    "LogicNativeValueChangedTo": {
        "inputs": {"Value": "NodeSocketLogicGeneric", "Target": "NodeSocketLogicGeneric"},
        "outputs": {"Result": "NodeSocketLogicExecution"},
    },
    "LogicNativeDelay": {
        "inputs": {"Flow": "NodeSocketLogicExecution", "Delay": "NodeSocketLogicFloat"},
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeTimer": {
        "inputs": {"Set Timer": "NodeSocketLogicExecution", "Seconds": "NodeSocketLogicFloat"},
        "outputs": {"When Elapsed": "NodeSocketLogicExecution"},
    },
    "LogicNativeCooldown": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Reset": "NodeSocketLogicExecution",
            "Duration": "NodeSocketLogicFloat",
            "Ignore Timescale": "NodeSocketLogicBool",
        },
        "outputs": {
            "Accepted": "NodeSocketLogicExecution",
            "Blocked": "NodeSocketLogicExecution",
            "Completed": "NodeSocketLogicExecution",
            "Remaining": "NodeSocketLogicFloat",
            "Progress": "NodeSocketLogicFloat",
            "Is Ready": "NodeSocketLogicBool",
        },
    },
    "LogicNativePulsify": {
        "inputs": {"Flow": "NodeSocketLogicExecution", "Gap": "NodeSocketLogicFloat"},
        "outputs": {"Pulse": "NodeSocketLogicExecution"},
    },
    "LogicNativeBarrier": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Condition": "NodeSocketLogicCondition",
            "Time": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeValueBool": {"outputs": {"Bool": "NodeSocketLogicBool"}},
    "LogicNativeValueInt": {"outputs": {"Int": "NodeSocketLogicInt"}},
    "LogicNativeValueFloat": {"outputs": {"Float": "NodeSocketLogicFloat"}},
    "LogicNativeRandomValue": {
        "inputs": {"Min": "NodeSocketLogicFloat", "Max": "NodeSocketLogicFloat"},
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeValueSwitch": {
        "inputs": {
            "Condition": "NodeSocketLogicBool",
            "True": "NodeSocketLogicFloat",
            "False": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeStoreValue": {
        "inputs": {"Flow": "NodeSocketLogicExecution", "Value": "NodeSocketLogicFloat"},
        "outputs": {"Done": "NodeSocketLogicExecution", "Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeValueString": {"outputs": {"String": "NodeSocketLogicString"}},
    "LogicNativeStringOperation": {
        "inputs": {
            "String": "NodeSocketLogicString",
            "Substring": "NodeSocketLogicString",
            "Replacement": "NodeSocketLogicString",
            "Length": "NodeSocketLogicInt",
        },
        "outputs": {
            "String": "NodeSocketLogicString",
            "Result": "NodeSocketLogicBool",
            "Count": "NodeSocketLogicInt",
        },
    },
    "LogicNativeFormattedString": {
        "inputs": {
            "Format String": "NodeSocketLogicString",
            "A": "NodeSocketLogicString",
            "B": "NodeSocketLogicString",
            "C": "NodeSocketLogicString",
            "D": "NodeSocketLogicString",
        },
        "outputs": {"String": "NodeSocketLogicString"},
    },
    "LogicNativeValueColor": {"outputs": {"Value": "NodeSocketLogicColor"}},
    "LogicNativeColorRGB": {
        "inputs": {
            "R": "NodeSocketLogicFloat",
            "G": "NodeSocketLogicFloat",
            "B": "NodeSocketLogicFloat",
        },
        "outputs": {"Color": "NodeSocketLogicColor"},
    },
    "LogicNativeColorRGBA": {
        "inputs": {
            "R": "NodeSocketLogicFloat",
            "G": "NodeSocketLogicFloat",
            "B": "NodeSocketLogicFloat",
            "A": "NodeSocketLogicFloat",
        },
        "outputs": {"Color": "NodeSocketLogicColor"},
    },
    "LogicNativeValueVector": {"outputs": {"Vector": "NodeSocketLogicVector"}},
    "LogicNativeEuler": {
        "inputs": {
            "X": "NodeSocketFloatAngle",
            "Y": "NodeSocketFloatAngle",
            "Z": "NodeSocketFloatAngle",
        },
        "outputs": {"Rotation": "NodeSocketLogicRotation"},
    },
    "LogicNativeBoneRotationToObjectRotation": {
        "inputs": {"Bone Rotation": "NodeSocketLogicRotation"},
        "outputs": {"Object Rotation": "NodeSocketLogicRotation"},
    },
    "LogicNativeCombineXY": {
        "inputs": {
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
        },
        "outputs": {"Vector": "NodeSocketLogicVector"},
    },
    "LogicNativeCombineXYZ": {
        "inputs": {
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
            "Z": "NodeSocketLogicFloat",
        },
        "outputs": {"Vector": "NodeSocketLogicVector"},
    },
    "LogicNativeSeparateXY": {
        "inputs": {"Vector": "NodeSocketLogicVector"},
        "outputs": {
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeSeparateEuler": {
        "inputs": {"Rotation": "NodeSocketLogicRotation"},
        "outputs": {
            "X": "NodeSocketFloatAngle",
            "Y": "NodeSocketFloatAngle",
            "Z": "NodeSocketFloatAngle",
        },
    },
    "LogicNativeSeparateXYZ": {
        "inputs": {"Vector": "NodeSocketLogicVector"},
        "outputs": {
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
            "Z": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeInvertValue": {
        "inputs": {"Value": "NodeSocketLogicFloat"},
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeClampValue": {
        "inputs": {
            "Value": "NodeSocketLogicFloat",
            "Min": "NodeSocketLogicFloat",
            "Max": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeLimitRange": {
        "inputs": {
            "Value": "NodeSocketLogicFloat",
            "Min": "NodeSocketLogicFloat",
            "Max": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeCompare": {
        "inputs": {"A": "NodeSocketLogicFloat", "B": "NodeSocketLogicFloat"},
        "outputs": {"Result": "NodeSocketLogicBool"},
    },
    "LogicNativeThreshold": {
        "inputs": {
            "Else 0": "NodeSocketLogicBool",
            "Value": "NodeSocketLogicFloat",
            "Threshold": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeRangedThreshold": {
        "inputs": {
            "Value": "NodeSocketLogicFloat",
            "Min": "NodeSocketLogicFloat",
            "Max": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeWithinRange": {
        "inputs": {
            "Value": "NodeSocketLogicFloat",
            "Min": "NodeSocketLogicFloat",
            "Max": "NodeSocketLogicFloat",
        },
        "outputs": {"Result": "NodeSocketLogicBool"},
    },
    "LogicNativeMapRange": {
        "inputs": {
            "Value": "NodeSocketLogicFloat",
            "From Min": "NodeSocketLogicFloat",
            "From Max": "NodeSocketLogicFloat",
            "To Min": "NodeSocketLogicFloat",
            "To Max": "NodeSocketLogicFloat",
        },
        "outputs": {"Result": "NodeSocketLogicFloat"},
    },
    "LogicNativeFloatCurve": {
        "inputs": {
            "Factor": "NodeSocketLogicFloat",
            "Value": "NodeSocketLogicFloat",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeMath": {
        "inputs": {"A": "NodeSocketLogicFloat", "B": "NodeSocketLogicFloat"},
        "outputs": {"Result": "NodeSocketLogicFloat"},
    },
    "LogicNativeVectorMath": {
        "inputs": {
            "A": "NodeSocketLogicVector",
            "B": "NodeSocketLogicVector",
            "Scale": "NodeSocketLogicFloat",
        },
        "outputs": {"Result": "NodeSocketLogicVector"},
    },
    "LogicNativeGetObjectAttribute": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {
            "Name": "NodeSocketLogicString",
            "Vector": "NodeSocketLogicVector",
            "Visible": "NodeSocketLogicBool",
            "Position": "NodeSocketLogicVector",
            "Orientation": "NodeSocketLogicRotation",
            "Scale": "NodeSocketLogicVector",
            "Color": "NodeSocketLogicColor",
        },
    },
    "LogicNativeGetDistance": {
        "inputs": {
            "Object A": "NodeSocketLogicObject",
            "Position A": "NodeSocketLogicVector",
            "Object B": "NodeSocketLogicObject",
            "Position B": "NodeSocketLogicVector",
        },
        "outputs": {
            "Distance": "NodeSocketLogicFloat",
            "Squared Distance": "NodeSocketLogicFloat",
            "Valid": "NodeSocketLogicCondition",
        },
    },
    "LogicNativeGetAverageCenterPosition": {
        "inputs": {
            "Vectors": "NodeSocketLogicList",
            "Objects": "NodeSocketLogicList",
            "Collection": "NodeSocketLogicCollection",
        },
        "outputs": {
            "Center": "NodeSocketLogicVector",
            "Count": "NodeSocketLogicInt",
            "Valid": "NodeSocketLogicCondition",
        },
    },
    "LogicNativePredictMotion": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Reset": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Position": "NodeSocketLogicVector",
            "Prediction Time": "NodeSocketLogicFloat",
            "Smoothing Time": "NodeSocketLogicFloat",
        },
        "outputs": {
            "Updated": "NodeSocketLogicExecution",
            "Position": "NodeSocketLogicVector",
            "Velocity": "NodeSocketLogicVector",
            "Direction": "NodeSocketLogicVector",
            "Speed": "NodeSocketLogicFloat",
            "Predicted Position": "NodeSocketLogicVector",
            "Valid": "NodeSocketLogicCondition",
        },
    },
    "LogicNativeGetLightColor": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {"Color": "NodeSocketLogicColor"},
    },
    "LogicNativeGetLightPower": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {"Power": "NodeSocketLogicFloat"},
    },
    "LogicNativeGetGravity": {"outputs": {"Gravity": "NodeSocketLogicVector"}},
    "LogicNativeGetTimescale": {"outputs": {"Timescale": "NodeSocketLogicFloat"}},
    "LogicNativeTime": {
        "outputs": {
            "Time": "NodeSocketLogicFloat",
            "Delta": "NodeSocketLogicFloat",
            "FPS": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeDeltaFactor": {"outputs": {"Factor": "NodeSocketLogicFloat"}},
    "LogicNativeQuitGame": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
    },
    "LogicNativeRestartGame": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeLoadBlendFile": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "File Name": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeStartLogicTree": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Tree Name": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeStopLogicTree": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Tree Name": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeRunLogicTree": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Tree Name": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeInstallLogicTree": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Tree Name": "NodeSocketLogicString",
            "Initialize": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeLogicTreeStatus": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Tree Name": "NodeSocketLogicString",
        },
        "outputs": {
            "Running": "NodeSocketLogicBool",
            "Stopped": "NodeSocketLogicBool",
        },
    },
    "LogicNativeGetActiveCamera": {"outputs": {"Camera": "NodeSocketLogicObject"}},
    "LogicNativeWorldToScreen": {
        "inputs": {
            "Point": "NodeSocketLogicVector",
            "Camera": "NodeSocketLogicObject",
        },
        "outputs": {"On Screen": "NodeSocketLogicVector"},
    },
    "LogicNativeScreenToWorld": {
        "inputs": {
            "Camera": "NodeSocketLogicObject",
            "Screen X": "NodeSocketLogicFloat",
            "Screen Y": "NodeSocketLogicFloat",
            "Depth": "NodeSocketLogicFloat",
        },
        "outputs": {"World Position": "NodeSocketLogicVector"},
    },
    "LogicNativeGetFullscreen": {"outputs": {"Fullscreen": "NodeSocketLogicBool"}},
    "LogicNativeGetResolution": {
        "outputs": {
            "Width": "NodeSocketLogicInt",
            "Height": "NodeSocketLogicInt",
            "Resolution": "NodeSocketLogicVector",
        },
    },
    "LogicNativeGetVSync": {"outputs": {"Mode": "NodeSocketLogicInt"}},
    "LogicNativeGetCollisionGroup": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {"Layers": "NodeSocketLogicInt"},
    },
    "LogicNativeSetObjectAttribute": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "XYZ": "NodeSocketLogicVector",
            "Object": "NodeSocketLogicObject",
            "Value": "NodeSocketLogicVector",
            "Position": "NodeSocketLogicVector",
            "Rotation": "NodeSocketLogicRotation",
            "Scale": "NodeSocketLogicVector",
            "Color": "NodeSocketLogicColor",
            "Visible": "NodeSocketLogicBool",
            "Include Children": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeApplyImpulse": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Attach": "NodeSocketLogicVector",
            "Impulse": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLightColor": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Color": "NodeSocketLogicColor",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetLightPower": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Power": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetLightShadow": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Use Shadow": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeMakeLightUnique": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeApplyMovement": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Vector": "NodeSocketLogicVector",
            "Local": "NodeSocketLogicBool",
        },
    },
    "LogicNativeApplyRotation": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Rotation": "NodeSocketLogicRotation",
            "Local": "NodeSocketLogicBool",
        },
    },
    "LogicNativeApplyForce": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Force": "NodeSocketLogicVector",
            "Local": "NodeSocketLogicBool",
        },
    },
    "LogicNativeApplyTorque": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Torque": "NodeSocketLogicVector",
            "Local": "NodeSocketLogicBool",
        },
    },
    "LogicNativeSetGravity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Gravity": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetTimescale": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Timescale": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeSetGamePaused": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Paused": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCamera": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCameraFov": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
            "FOV": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCameraOrthoScale": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
            "Scale": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetFullscreen": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Fullscreen": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetResolution": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "X": "NodeSocketLogicInt",
            "Y": "NodeSocketLogicInt",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetVSync": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeShowFramerate": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Show": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeShowProfile": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Show": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCollisionGroup": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Layers": "NodeSocketLogicCollisionLayers",
        },
    },
    "LogicNativeSetPhysics": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Active": "NodeSocketLogicBool",
        },
    },
    "LogicNativeSetPhysicsPaused": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Paused": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetDynamics": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Enabled": "NodeSocketLogicBool",
        },
    },
    "LogicNativeRebuildCollisionShape": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
        },
    },
    "LogicNativeGetRigidBodyAttribute": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {
            "Valid": "NodeSocketLogicBool",
            "Value": "NodeSocketLogicFloat",
            "Linear Damping": "NodeSocketLogicFloat",
            "Angular Damping": "NodeSocketLogicFloat",
            "Enabled": "NodeSocketLogicBool",
            "Allow Sleeping": "NodeSocketLogicBool",
            "Lock Translation": "NodeSocketLogicVector",
            "Lock Rotation": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetRigidBodyAttribute": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Value": "NodeSocketLogicFloat",
            "Linear Damping": "NodeSocketLogicFloat",
            "Angular Damping": "NodeSocketLogicFloat",
            "Enabled": "NodeSocketLogicBool",
            "Allow Sleeping": "NodeSocketLogicBool",
            "Wake": "NodeSocketLogicBool",
            "Lock Translation": "NodeSocketLogicVector",
            "Lock Rotation": "NodeSocketLogicVector",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeGetCharacterInfo": {
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {
            "Max Jumps": "NodeSocketLogicInt",
            "Current Jump Count": "NodeSocketLogicInt",
            "Gravity": "NodeSocketLogicVector",
            "Walk Direction": "NodeSocketLogicVector",
            "On Ground": "NodeSocketLogicBool",
        },
    },
    "LogicNativeCharacterJump": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCharacterGravity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Gravity": "NodeSocketLogicVector",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCharacterJumpSpeed": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Force": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCharacterMaxJumps": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Max Jumps": "NodeSocketLogicInt",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCharacterWalkDirection": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Vector": "NodeSocketLogicVector",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetCharacterVelocity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Velocity": "NodeSocketLogicVector",
            "Time": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeVehicleControl": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Vehicle": "NodeSocketLogicObject",
            "Throttle": "NodeSocketLogicFloat",
            "Brake": "NodeSocketLogicFloat",
            "Handbrake": "NodeSocketLogicFloat",
            "Steering": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeVehicleAccelerate": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Vehicle": "NodeSocketLogicObject",
            "Wheels": "NodeSocketLogicInt",
            "Power": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeVehicleBrake": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Vehicle": "NodeSocketLogicObject",
            "Wheels": "NodeSocketLogicInt",
            "Power": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeVehicleSteer": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Vehicle": "NodeSocketLogicObject",
            "Wheels": "NodeSocketLogicInt",
            "Steer": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeVehicleSetAttributes": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Vehicle": "NodeSocketLogicObject",
            "Wheels": "NodeSocketLogicInt",
            "Suspension": "NodeSocketLogicBool",
            "Suspension Value": "NodeSocketLogicFloat",
            "Stiffness": "NodeSocketLogicBool",
            "Stiffness Value": "NodeSocketLogicFloat",
            "Damping": "NodeSocketLogicBool",
            "Damping Value": "NodeSocketLogicFloat",
            "Friction": "NodeSocketLogicBool",
            "Friction Value": "NodeSocketLogicFloat",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeNone": {
        "inputs": {"Value": "NodeSocketLogicGeneric"},
        "outputs": {"If None": "NodeSocketLogicCondition"},
    },
    "LogicNativeNotNone": {
        "inputs": {"Value": "NodeSocketLogicGeneric"},
        "outputs": {"If Not None": "NodeSocketLogicCondition"},
    },
    "LogicNativeKeyboardKey": {
        "inputs": {"Key": "NodeSocketLogicString"},
        "outputs": {
            "Out": "NodeSocketLogicExecution",
            "Active": "NodeSocketLogicCondition",
        },
        "rna_enums": {
            "input_type": {"TAP", "DOWN", "UP"},
        },
    },
    "LogicNativeKeyboardActive": {
        "outputs": {"Active": "NodeSocketLogicCondition"},
    },
    "LogicNativeKeyCode": {
        "inputs": {"Key": "NodeSocketLogicString"},
        "outputs": {"Code": "NodeSocketLogicInt"},
    },
    "LogicNativeMouseButton": {
        "inputs": {"Button": "NodeSocketLogicString"},
        "outputs": {
            "Out": "NodeSocketLogicExecution",
            "Active": "NodeSocketLogicCondition",
        },
        "rna_enums": {},
    },
    "LogicNativeMouseMoved": {
        "outputs": {"If Moved": "NodeSocketLogicExecution"},
        "rna_enums": {},
        "rna_bools": {"pulse": False},
    },
    "LogicNativeMouseWheel": {
        "outputs": {
            "When Scrolled": "NodeSocketLogicExecution",
            "Difference": "NodeSocketLogicInt",
        },
        "rna_enums": {"wheel_direction": {"SCROLL_UP", "SCROLL_DOWN", "SCROLL_UP_OR_DOWN"}},
    },
    "LogicNativeMouseOver": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Camera": "NodeSocketLogicObject",
        },
        "outputs": {
            "On Enter": "NodeSocketLogicExecution",
            "On Over": "NodeSocketLogicExecution",
            "On Exit": "NodeSocketLogicExecution",
            "Entered": "NodeSocketLogicCondition",
            "Is Over": "NodeSocketLogicCondition",
            "Exited": "NodeSocketLogicCondition",
            "Object": "NodeSocketLogicObject",
            "Point": "NodeSocketLogicVector",
            "Normal": "NodeSocketLogicVector",
        },
        "rna_enums": {
            "interaction_view": {"AUTOMATIC", "MAIN", "OVERLAY", "CUSTOM_CAMERA"},
        },
    },
    "LogicNativeObjectScrollView": {
        "inputs": {
            "Viewport Object": "NodeSocketLogicObject",
            "Content Object": "NodeSocketLogicObject",
            "Camera": "NodeSocketLogicObject",
            "Minimum Offset": "NodeSocketLogicVector",
            "Maximum Offset": "NodeSocketLogicVector",
            "Wheel Step": "NodeSocketLogicVector",
            "Enabled": "NodeSocketLogicBool",
            "Horizontal": "NodeSocketLogicBool",
            "Vertical": "NodeSocketLogicBool",
            "Cull Outside": "NodeSocketLogicBool",
            "Focus Follow": "NodeSocketLogicBool",
            "User": "NodeSocketLogicInt",
            "Priority": "NodeSocketLogicInt",
            "Padding": "NodeSocketLogicFloat",
            "Scrollbar": "NodeSocketLogicObject",
        },
        "outputs": {
            "On Scroll": "NodeSocketLogicExecution",
            "At Start": "NodeSocketLogicCondition",
            "At End": "NodeSocketLogicCondition",
            "Offset": "NodeSocketLogicVector",
            "Normalized": "NodeSocketLogicVector",
        },
        "rna_enums": {"interaction_view": {"AUTOMATIC", "MAIN", "OVERLAY", "CUSTOM_CAMERA"}},
    },
    "LogicNativeObjectGridView": {
        "inputs": {
            "Viewport Object": "NodeSocketLogicObject",
            "Camera": "NodeSocketLogicObject",
            "Slots": "NodeSocketLogicGeneric",
            "Items": "NodeSocketLogicGeneric",
            "First Item": "NodeSocketLogicInt",
            "Columns": "NodeSocketLogicInt",
            "Wheel Rows": "NodeSocketLogicInt",
            "Enabled": "NodeSocketLogicBool",
            "Cull Outside": "NodeSocketLogicBool",
            "Focus Follow": "NodeSocketLogicBool",
            "User": "NodeSocketLogicInt",
            "Priority": "NodeSocketLogicInt",
            "Padding": "NodeSocketLogicFloat",
            "Icon Library": "NodeSocketLogicCollection",
            "Show Counts": "NodeSocketLogicBool",
            "Slot Template": "NodeSocketLogicObject",
            "Rows": "NodeSocketLogicInt",
            "Spacing": "NodeSocketLogicVector",
            "Scrollbar": "NodeSocketLogicObject",
        },
        "outputs": {
            "On Scroll": "NodeSocketLogicExecution",
            "On Rebind": "NodeSocketLogicExecution",
            "At Start": "NodeSocketLogicCondition",
            "At End": "NodeSocketLogicCondition",
            "First Visible": "NodeSocketLogicInt",
            "Last Visible": "NodeSocketLogicInt",
            "Normalized": "NodeSocketLogicVector",
        },
        "rna_enums": {"interaction_view": {"AUTOMATIC", "MAIN", "OVERLAY", "CUSTOM_CAMERA"}},
    },
    "LogicNativeInputAction": {
        "inputs": {
            "Action": "NodeSocketLogicString",
            "User": "NodeSocketLogicInt",
        },
        "outputs": {
            "Out": "NodeSocketLogicExecution",
            "Active": "NodeSocketLogicCondition",
            "Pressed": "NodeSocketLogicCondition",
            "Released": "NodeSocketLogicCondition",
            "Value": "NodeSocketLogicFloat",
            "Vector": "NodeSocketLogicVector",
        },
        "rna_enums": {
            "input_type": {"TAP", "DOWN", "UP"},
        },
    },
    "LogicNativeGetInputBinding": {
        "inputs": {"Action": "NodeSocketLogicString"},
        "outputs": {"Text": "NodeSocketLogicString"},
        "rna_enums": {
            "input_device": {"AUTOMATIC", "KEYBOARD_MOUSE", "GAMEPAD"},
            "input_direction": {"UP", "DOWN", "LEFT", "RIGHT"},
        },
    },
    "LogicNativeRebindInputAction": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Action": "NodeSocketLogicString",
            "User": "NodeSocketLogicInt",
            "Timeout": "NodeSocketLogicFloat",
        },
        "outputs": {
            "Done": "NodeSocketLogicExecution",
            "Success": "NodeSocketLogicBool",
        },
        "rna_enums": {
            "rebind_device": {"KEYBOARD_MOUSE", "GAMEPAD"},
            "input_direction": {"UP", "DOWN", "LEFT", "RIGHT"},
        },
    },
    "LogicNativeInputMap": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Path": "NodeSocketLogicString",
            "File": "NodeSocketLogicString",
        },
        "outputs": {
            "Done": "NodeSocketLogicExecution",
            "Success": "NodeSocketLogicBool",
        },
        "rna_enums": {
            "input_map_operation": {"SAVE", "LOAD", "RESET"},
        },
    },
    "LogicNativeSetOverlayCollection": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
            "Collection": "NodeSocketLogicCollection",
        },
        "outputs": {
            "Success": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
            "Result": "NodeSocketLogicInt",
        },
    },
    "LogicNativeRemoveOverlayCollection": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Collection": "NodeSocketLogicCollection",
        },
        "outputs": {
            "Success": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
            "Result": "NodeSocketLogicInt",
        },
    },
    "LogicNativeOpenObjectMenu": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
            "Collection": "NodeSocketLogicCollection",
            "Input Context": "NodeSocketLogicString",
        },
        "outputs": {
            "Opened": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
            "Menu": "NodeSocketLogicInt",
            "Result": "NodeSocketLogicInt",
        },
        "rna_enums": {
            "menu_routing": {"MODAL", "PASS_THROUGH"},
            "menu_open_mode": {"PUSH", "REPLACE_TOP"},
            "menu_presentation": {"SCREEN", "OVERLAY"},
            "menu_back_behavior": {"CLOSE_MENU", "IGNORE", "CUSTOM"},
        },
    },
    "LogicNativeCloseObjectMenu": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Menu": "NodeSocketLogicInt",
        },
        "outputs": {
            "Closed": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
            "Result": "NodeSocketLogicInt",
        },
        "rna_enums": {"menu_close_operation": {"TOP", "SPECIFIC", "ALL"}},
    },
    "LogicNativeSetUIFocus": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "User": "NodeSocketLogicInt",
            "Input Context": "NodeSocketLogicString",
        },
        "outputs": {
            "Done": "NodeSocketLogicExecution",
            "Failed": "NodeSocketLogicExecution",
            "Result": "NodeSocketLogicInt",
        },
    },
    "LogicNativeGetUIFocus": {
        "inputs": {
            "User": "NodeSocketLogicInt",
            "Input Context": "NodeSocketLogicString",
        },
        "outputs": {"Object": "NodeSocketLogicObject"},
    },
    "LogicNativeScreenAnchor": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Camera": "NodeSocketLogicObject",
            "Offset Pixels": "NodeSocketLogicVector",
            "Safe Margin Pixels": "NodeSocketLogicVector",
            "Depth": "NodeSocketLogicFloat",
            "Reference Resolution": "NodeSocketLogicVector",
            "Scale Offset": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
        "rna_enums": {
            "screen_anchor": {
                "TOP_LEFT", "TOP", "TOP_RIGHT", "LEFT", "CENTER", "RIGHT",
                "BOTTOM_LEFT", "BOTTOM", "BOTTOM_RIGHT",
            },
        },
    },
    "LogicNativeMouseRay": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Camera": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "X-Ray": "NodeSocketLogicBool",
            "Distance": "NodeSocketLogicFloat",
            "Mask": "NodeSocketLogicCollisionLayers",
        },
        "outputs": {
            "Has Result": "NodeSocketLogicCondition",
            "Picked Object": "NodeSocketLogicObject",
            "Picked Point": "NodeSocketLogicVector",
            "Picked Normal": "NodeSocketLogicVector",
        },
        "rna_enums": {
            "interaction_view": {"AUTOMATIC", "MAIN", "OVERLAY", "CUSTOM_CAMERA"},
        },
    },
    "LogicNativeCursorPosition": {
        "outputs": {
            "Position": "NodeSocketLogicVector",
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
        },
        "rna_bools": {"invert_y": False},
    },
    "LogicNativeCursorMovement": {
        "outputs": {
            "Movement": "NodeSocketLogicVector",
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeGamepadActive": {
        "inputs": {"Index": "NodeSocketLogicInt"},
        "outputs": {"Active": "NodeSocketLogicCondition"},
    },
    "LogicNativeGamepadButton": {
        "inputs": {
            "Index": "NodeSocketLogicInt",
        },
        "outputs": {
            "Out": "NodeSocketLogicExecution",
            "Active": "NodeSocketLogicCondition",
            "Strength": "NodeSocketLogicFloat",
        },
        "rna_enums": {
            "input_type": {"TAP", "DOWN", "UP"},
            "gamepad_button": {
                "A_CROSS",
                "B_CIRCLE",
                "X_SQUARE",
                "Y_TRIANGLE",
                "LB_L1",
                "RB_R1",
                "LT_L2",
                "RT_R2",
                "L3",
                "R3",
                "DPAD_UP",
                "DPAD_DOWN",
                "DPAD_LEFT",
                "DPAD_RIGHT",
                "SELECT_SHARE",
                "START_OPTIONS",
            },
        },
    },
    "LogicNativeGamepadStick": {
        "inputs": {
            "Invert X": "NodeSocketLogicBool",
            "Invert Y": "NodeSocketLogicBool",
            "Index": "NodeSocketLogicInt",
            "Sensitivity": "NodeSocketLogicFloat",
            "Threshold": "NodeSocketLogicFloat",
        },
        "outputs": {
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
            "VEC": "NodeSocketLogicVector",
        },
        "rna_enums": {"axis": {"LEFT_STICK", "RIGHT_STICK"}},
    },
    "LogicNativeGamepadLook": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Body Object": "NodeSocketLogicObject",
            "Head Object": "NodeSocketLogicObject",
            "Inverted": "NodeSocketLogicVector",
            "Index": "NodeSocketLogicInt",
            "Sensitivity": "NodeSocketLogicFloat",
            "Exponent": "NodeSocketLogicFloat",
            "Cap Left / Right": "NodeSocketLogicBool",
            "Left / Right Range": "NodeSocketLogicVector",
            "Cap Up / Down": "NodeSocketLogicBool",
            "Up / Down Range": "NodeSocketLogicVector",
            "Threshold": "NodeSocketLogicFloat",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
        "rna_enums": {"axis": {"LEFT_STICK", "RIGHT_STICK"}},
    },
    "LogicNativeGetGamePropertyFloat": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
        },
        "outputs": {"Value": "NodeSocketLogicFloat"},
    },
    "LogicNativeGetGamePropertyBool": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
        },
        "outputs": {"Value": "NodeSocketLogicBool"},
    },
    "LogicNativeGetGamePropertyInt": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
        },
        "outputs": {"Value": "NodeSocketLogicInt"},
    },
    "LogicNativeGetGamePropertyString": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
        },
        "outputs": {"Value": "NodeSocketLogicString"},
    },
    "LogicNativeHasProperty": {
        "inputs": {
            "Object": "NodeSocketLogicObject",
            "Name": "NodeSocketLogicString",
        },
        "outputs": {"Out": "NodeSocketLogicCondition"},
    },
    "LogicNativeGetTreeProperty": {
        "inputs": {"Property": "NodeSocketLogicString"},
        "outputs": {"Value": "NodeSocketLogicGeneric"},
    },
    "LogicNativeSetGamePropertyFloat": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicFloat",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetGamePropertyBool": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicBool",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetGamePropertyInt": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicInt",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetGamePropertyString": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetTreeProperty": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicGeneric",
        },
    },
    "LogicNativeSetCursorVisibility": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Visible": "NodeSocketLogicBool",
        },
    },
    "LogicNativeSetCursorPosition": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Screen X": "NodeSocketLogicFloat",
            "Screen Y": "NodeSocketLogicFloat",
        },
    },
    "LogicNativeGamepadVibration": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Index": "NodeSocketLogicInt",
            "Left": "NodeSocketLogicFloat",
            "Right": "NodeSocketLogicFloat",
            "Time": "NodeSocketLogicFloat",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeToggleProperty": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeModifyProperty": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicFloat",
        },
        "outputs": {"Done": "NodeSocketLogicExecution"},
    },
    "LogicNativeAddObject": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object to Add": "NodeSocketLogicObject",
            "Copy Transform": "NodeSocketLogicObject",
            "Life": "NodeSocketLogicFloat",
            "Full Copy": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeSetParent": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Child Object": "NodeSocketLogicObject",
            "Parent Object": "NodeSocketLogicObject",
            "Bone Name": "NodeSocketLogicString",
            "Compound": "NodeSocketLogicBool",
            "Ghost": "NodeSocketLogicBool",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeGetParent": {
        "inputs": {"Child Object": "NodeSocketLogicObject"},
        "outputs": {"Parent Object": "NodeSocketLogicObject"},
    },
    "LogicNativeGetChild": {
        "inputs": {
            "Parent Object": "NodeSocketLogicObject",
            "Index": "NodeSocketLogicInt",
        },
        "outputs": {"Child Object": "NodeSocketLogicObject"},
    },
    "LogicNativeRemoveParent": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Child Object": "NodeSocketLogicObject",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeRemoveObject": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
        },
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeRaycast": {
        "inputs": {
            "Caster": "NodeSocketLogicObject",
            "Origin": "NodeSocketLogicVector",
            "Destination": "NodeSocketLogicVector",
            "Local": "NodeSocketLogicBool",
            "Mask": "NodeSocketLogicCollisionLayers",
        },
        "outputs": {
            "Has Result": "NodeSocketLogicCondition",
            "Picked Object": "NodeSocketLogicObject",
            "Point": "NodeSocketLogicVector",
            "Normal": "NodeSocketLogicVector",
            "Direction": "NodeSocketLogicVector",
        },
    },
    "LogicNativeRaycastFromMesh": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Source Object": "NodeSocketLogicObject",
            "Ignore Object": "NodeSocketLogicObject",
            "Start Offset": "NodeSocketLogicFloat",
            "Max Distance": "NodeSocketLogicFloat",
            "Max Rays": "NodeSocketLogicInt",
            "Property": "NodeSocketLogicString",
            "X-Ray": "NodeSocketLogicBool",
            "Mask": "NodeSocketLogicCollisionLayers",
            "Include Sensors": "NodeSocketLogicBool",
            "Hit Backfaces": "NodeSocketLogicBool",
            "Visualize": "NodeSocketLogicBool",
        },
        "outputs": {
            "Done": "NodeSocketLogicExecution",
            "Any Hit": "NodeSocketLogicCondition",
            "All Hit": "NodeSocketLogicCondition",
            "Hit Count": "NodeSocketLogicInt",
            "Ray Count": "NodeSocketLogicInt",
            "Hits": "NodeSocketLogicList",
            "Blocked": "NodeSocketLogicList",
            "Hit Objects": "NodeSocketLogicList",
            "Points": "NodeSocketLogicList",
            "Normals": "NodeSocketLogicList",
            "Distances": "NodeSocketLogicList",
            "Fractions": "NodeSocketLogicList",
            "Face Indices": "NodeSocketLogicList",
            "Has UVs": "NodeSocketLogicList",
            "UVs": "NodeSocketLogicList",
            "Ray Origins": "NodeSocketLogicList",
            "Ray Directions": "NodeSocketLogicList",
            "End Points": "NodeSocketLogicList",
        },
    },
}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)

    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    return parser.parse_args(argv)


def new_logic_tree(name):
    tree = bpy.data.node_groups.new(name, "LogicNodeTree")
    tree.use_fake_user = True
    return tree


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def socket_by_identifier(sockets, identifier, node_idname):
    socket = next((socket for socket in sockets if socket.identifier == identifier), None)
    require(socket is not None, f"{node_idname} is missing socket {identifier}")
    return socket


def unsupported_compiler_node_idnames():
    repo_root = Path(__file__).resolve().parents[2]
    compiler_path = repo_root / "source/gameengine/LogicNodes/intern/LN_TreeCompiler.cpp"
    compiler_text = compiler_path.read_text(encoding="utf-8")
    return {
        f"LogicNative{match.group(1)}"
        for match in UNSUPPORTED_COMMAND_HANDLER_RE.finditer(compiler_text)
    }


def require_color_close(actual, expected, label):
    require(len(actual) == 4, f"{label} returned a non-RGBA color: {actual}")
    for actual_value, expected_value in zip(actual, expected):
        require(
            abs(actual_value - expected_value) < 0.0005,
            f"{label} color mismatch: expected {expected}, got {tuple(actual)}",
        )


class MenuProbeLayout:
    def __init__(self):
        self.operator_context = "INVOKE_REGION_WIN"
        self.menu_calls = []
        self.menu_contents_calls = []
        self.operator_calls = []
        self.separator_count = 0

    def menu(self, menu_idname, **_kwargs):
        self.menu_calls.append(menu_idname)
        return None

    def menu_contents(self, menu_idname):
        self.menu_contents_calls.append(menu_idname)

    def separator(self):
        self.separator_count += 1

    def operator(self, operator_idname, **kwargs):
        settings_items = []
        operator_props = type("LogicNodeMenuOperatorProps", (), {})()
        operator_props.operator_idname = operator_idname

        class LogicNodeMenuOperatorSettings:
            def add(self_inner):
                setting = type("LogicNodeMenuOperatorSetting", (), {})()
                setting.name = ""
                setting.value = ""
                settings_items.append(setting)
                return setting

        operator_props.settings = LogicNodeMenuOperatorSettings()
        operator_props.settings_items = settings_items
        operator_props.text = kwargs.get("text", "")
        operator_props.kwargs = kwargs
        operator_props.type = None
        operator_props.use_transform = False
        self.operator_calls.append(operator_props)
        return operator_props


def menu_probe_context(tree_type):
    space_data = type("LogicNodeMenuSpaceData", (), {
        "tree_type": tree_type,
        "type": "NODE_EDITOR",
    })()
    return type(
        "LogicNodeMenuContext",
        (),
        {
            "space_data": space_data,
            "is_menu_search": False,
        },
    )()


def find_menu_class(module, bl_idname):
    for cls in module.classes:
        if getattr(cls, "bl_idname", None) == bl_idname:
            return cls
    return None


def build_menu_probe(menu_cls, layout):
    probe_type = type(
        f"{menu_cls.__name__}Probe",
        (),
        {
            "pathing_dict": menu_cls.pathing_dict,
            "main_operator_id": getattr(menu_cls, "main_operator_id", ""),
            "use_transform": getattr(menu_cls, "use_transform", False),
            "draw_menu": classmethod(node_add_menu.NodeMenu.draw_menu.__func__),
            "node_operator": classmethod(node_add_menu.NodeMenu.node_operator.__func__),
            "node_operator_with_searchable_enum": classmethod(
                node_add_menu.NodeMenu.node_operator_with_searchable_enum.__func__),
        },
    )
    probe = probe_type()
    probe.layout = layout
    return probe


def draw_menu_layout(logic_menu_module, menu_cls, context):
    del logic_menu_module
    layout = MenuProbeLayout()
    menu_probe = build_menu_probe(menu_cls, layout)
    menu_cls.draw(menu_probe, context)
    return layout


def operator_types(layout):
    return [operator_call.type for operator_call in layout.operator_calls]


def validate_editor_menus():
    require(
        "node_add_menu_logic" in bl_ui._modules,
        "LogicNodeTree menu module is not listed in bl_ui startup modules",
    )
    require(hasattr(bl_ui, "node_add_menu_logic"), "LogicNodeTree menu module was not imported")

    logic_menu_module = bl_ui.node_add_menu_logic
    logic_add_menu_cls = find_menu_class(logic_menu_module, "NODE_MT_logic_node_add_all")
    logic_swap_menu_cls = find_menu_class(logic_menu_module, "NODE_MT_logic_node_swap_all")
    require(logic_add_menu_cls is not None, "LogicNodeTree add root menu class is missing")
    require(logic_swap_menu_cls is not None, "LogicNodeTree swap root menu class is missing")

    context = menu_probe_context("LogicNodeTree")

    require(
        set(logic_add_menu_cls.pathing_dict) == set(logic_swap_menu_cls.pathing_dict),
        "LogicNodeTree add/swap menu path maps diverged",
    )

    add_layout = MenuProbeLayout()
    space_node.NODE_MT_add.draw(type("LogicNodeAddProbe", (), {"layout": add_layout})(), context)
    require(
        add_layout.menu_contents_calls == ["NODE_MT_logic_node_add_all"],
        f"LogicNodeTree add menu did not dispatch to the Logic root menu: {add_layout.menu_contents_calls}",
    )

    swap_layout = MenuProbeLayout()
    space_node.NODE_MT_swap.draw(type("LogicNodeSwapProbe", (), {"layout": swap_layout})(), context)
    require(
        swap_layout.menu_contents_calls == ["NODE_MT_logic_node_swap_all"],
        f"LogicNodeTree swap menu did not dispatch to the Logic root menu: {swap_layout.menu_contents_calls}",
    )

    expected_root_paths = list(logic_menu_module.LOGIC_ROOT_MENU_PATHS)
    expected_root_menus = [logic_add_menu_cls.pathing_dict[path] for path in expected_root_paths]
    add_root_layout = draw_menu_layout(logic_menu_module, logic_add_menu_cls, context)
    require(
        add_root_layout.menu_calls == expected_root_menus,
        f"LogicNodeTree add root menu does not match addon-style ordering: {add_root_layout.menu_calls}",
    )
    require(
        add_root_layout.separator_count == len(logic_menu_module.LOGIC_ROOT_MENU_GROUPS) - 1,
        f"LogicNodeTree add root menu has wrong separator count: {add_root_layout.separator_count}",
    )

    expected_swap_menus = [logic_swap_menu_cls.pathing_dict[path] for path in expected_root_paths]
    swap_root_layout = draw_menu_layout(logic_menu_module, logic_swap_menu_cls, context)
    require(
        swap_root_layout.menu_calls == expected_swap_menus,
        f"LogicNodeTree swap root menu does not match addon-style ordering: {swap_root_layout.menu_calls}",
    )
    require(
        swap_root_layout.separator_count == len(logic_menu_module.LOGIC_ROOT_MENU_GROUPS) - 1,
        f"LogicNodeTree swap root menu has wrong separator count: {swap_root_layout.separator_count}",
    )

    expected_menu_contents = {
        "Events": {
            "nodes": [
                "LogicNativeOnInit",
                "LogicNativeOnGameLoaded",
                "LogicNativeOnUpdate",
                "LogicNativeWhilePaused",
                "LogicNativeOnFixedUpdate",
                "LogicNativeValueChangedTo",
                "LogicNativeValueChanged",
                "LogicNativeBooleanEdge",
                "LogicNativeOnCollision",
                "LogicNativeOnConstraintBreak",
                "LogicNativeOnce",
            ],
            "paths": ["Events/Custom"],
            "separators": 1,
        },
        "Events/Custom": {
            "nodes": [
                "LogicNativeSendEvent",
                "LogicNativeReceiveEvent",
            ],
        },
        "Game": {
            "nodes": [
                "LogicNativeLoadBlendFile",
                "LogicNativeQuitGame",
                "LogicNativeRestartGame",
                "LogicNativeSaveGame",
                "LogicNativeLoadGame",
                "LogicNativeSetSaveExistence",
            ],
            "separators": 1,
        },
        "Input": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Input"]),
        },
        "Input/Input Map": {
            "nodes": [
                "LogicNativeInputAction",
                "LogicNativeGetInputBinding",
                "LogicNativeRebindInputAction",
                "LogicNativeInputMap",
            ],
            "separators": 1,
        },
        "Input/Mouse": {
            "nodes": [
                "LogicNativeMouseButton",
                "LogicNativeMouseWheel",
                "LogicNativeMouseMoved",
                "LogicNativeMouseOver",
                "LogicNativeMouseRay",
                "LogicNativeSetCursorVisibility",
                "LogicNativeCursorPosition",
                "LogicNativeSetCursorPosition",
                "LogicNativeCursorMovement",
                "LogicNativeMouseLook",
            ],
            "separators": 2,
        },
        "Input/Keyboard": {
            "nodes": [
                "LogicNativeKeyboardKey",
                "LogicNativeKeyboardActive",
                "LogicNativeKeyCode",
                "LogicNativeKeyLogger",
            ],
            "separators": 1,
        },
        "Input/Gamepad": {
            "nodes": [
                "LogicNativeGamepadButton",
                "LogicNativeGamepadStick",
                "LogicNativeGamepadActive",
                "LogicNativeGamepadVibration",
                "LogicNativeGamepadLook",
            ],
            "separators": 1,
        },
        "Values": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Values"]),
            "nodes": [
                "LogicNativeValueBool",
                "LogicNativeValueInt",
                "LogicNativeValueFloat",
                "LogicNativeValueString",
                "LogicNativeStringOperation",
                "LogicNativeFormattedString",
                "LogicNativeRandomValue",
                "LogicNativeRandomFloat",
                "LogicNativeRandomInt",
                "LogicNativeTypecast",
                "LogicNativeMixValue",
                "LogicNativeValueSwitch",
                "LogicNativeValueSwitchList",
                "LogicNativeValueSwitchListCompare",
                "LogicNativeStoreValue",
                "LogicNativeInvertValue",
                "LogicNativeLimitRange",
            ],
            "separators": 2,
        },
        "Values/Vector": {
            "nodes": [
                "LogicNativeValueColor",
                "LogicNativeColorRGB",
                "LogicNativeColorRGBA",
                "LogicNativeValueVector",
                "LogicNativeSeparateXY",
                "LogicNativeSeparateXYZ",
                "LogicNativeCombineXY",
                "LogicNativeCombineXYZ",
                "LogicNativeCombineXYZW",
                "LogicNativeRandomVector",
                "LogicNativeResizeVector",
                "LogicNativeXYZToMatrix",
                "LogicNativeMatrixToXYZ",
                "LogicNativeVectorRotate",
            ],
            "separators": 1,
        },
        "Values/Rotation": {
            "nodes": [
                "LogicNativeEuler",
                "LogicNativeSeparateEuler",
                "LogicNativeVectorToRotation",
                "LogicNativeBoneRotationToObjectRotation",
            ],
            "separators": 1,
        },
        "Values/Properties": {
            "nodes": [
                "LogicNativeHasProperty",
                "LogicNativeEvaluateProperty",
                "LogicNativeGetGameProperty",
                "LogicNativeGetGamePropertyFloat",
                "LogicNativeGetGamePropertyInt",
                "LogicNativeGetGamePropertyBool",
                "LogicNativeGetGamePropertyString",
                "LogicNativeGetTreeProperty",
                "LogicNativeGetGlobalProperty",
                "LogicNativeListGlobalProperties",
                "LogicNativeSetGameProperty",
                "LogicNativeSetGamePropertyFloat",
                "LogicNativeSetGamePropertyInt",
                "LogicNativeSetGamePropertyBool",
                "LogicNativeSetGamePropertyString",
                "LogicNativeSetTreeProperty",
                "LogicNativeToggleTreeProperty",
                "LogicNativeSetGlobalProperty",
                "LogicNativeLoadVariable",
                "LogicNativeSaveVariable",
                "LogicNativeLoadVariableDict",
                "LogicNativeSaveVariableDict",
                "LogicNativeClearVariables",
                "LogicNativeListSavedVariables",
                "LogicNativeRemoveVariable",
                "LogicNativeToggleProperty",
                "LogicNativeModifyProperty",
                "LogicNativeCopyProperty",
            ],
            "separators": 5,
        },
        "Animation": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Animation"]),
        },
        "Animation/Actions": {
            "nodes": [
                "LogicNativePlayAction",
                "LogicNativeStopAction",
                "LogicNativeSetActionFrame",
                "LogicNativeActionDone",
                "LogicNativeAnimationStatus",
            ],
            "separators": 1,
        },
        "Animation/Get Rest Bone Data": {
            "nodes": [
                "LogicNativeGetBoneHeadWorld",
                "LogicNativeGetBoneTailWorld",
                "LogicNativeGetBoneLength",
                "LogicNativeGetBoneCenterWorld",
            ],
        },
        "Animation/Get Pose Bone Data": {
            "nodes": [
                "LogicNativeGetBoneHeadPoseWorld",
                "LogicNativeGetBoneTailPoseWorld",
                "LogicNativeGetBoneCenterPoseWorld",
                "LogicNativeGetBonePoseRotation",
                "LogicNativeGetBonePoseScale",
                "LogicNativeGetBonePoseTransform",
            ],
        },
        "Animation/Set Pose Bone Data": {
            "nodes": [
                "LogicNativeSetBonePoseLocation",
                "LogicNativeSetBonePoseRotation",
                "LogicNativeSetBonePoseScale",
                "LogicNativeSetBonePoseTransform",
            ],
        },
        "Animation/Advanced Bone Data": {
            "nodes": [
                "LogicNativeGetBoneAttribute",
                "LogicNativeSetBoneAttribute",
            ],
        },
        "Animation/Bone Constraints": {
            "nodes": [
                "LogicNativeSetBoneConstraintInfluence",
                "LogicNativeSetBoneConstraintTarget",
                "LogicNativeSetBoneConstraintAttribute",
            ],
        },
        "Lights": {
            "nodes": [
                "LogicNativeGetLightColor",
                "LogicNativeGetLightPower",
                "LogicNativeMakeLightUnique",
                "LogicNativeSetLightColor",
                "LogicNativeSetLightPower",
                "LogicNativeSetLightShadow",
            ],
            "separators": 1,
        },
        "Nodes": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Nodes"]),
            "nodes": [
                "LogicNativeGetEditorNodeValue",
                "LogicNativeSetEditorNodeValue",
                "LogicNativeMakeNodeTreeUnique",
                "LogicNativeSetNodeMute",
            ],
            "separators": 1,
        },
        "Nodes/Materials": {
            "nodes": [
                "LogicNativeGetMaterialFromSlot",
                "LogicNativeGetMaterialSlotCount",
                "LogicNativeGetMaterialName",
                "LogicNativeGetMaterialParameter",
                "LogicNativeSetMaterialSlot",
                "LogicNativeSetMaterialParameter",
            ],
            "separators": 1,
        },
        "Nodes/Geometry": {
            "nodes": [
                "LogicNativeSetGeometryNodesInput",
                "LogicNativeEnableDisableModifier",
                "LogicNativeAssignGeometryNodesModifier",
            ],
        },
        "Nodes/Groups": {
            "nodes": [],
        },
        "Objects": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Objects"]),
            "nodes": [
                "LogicNativeGetObjectAttribute",
                "LogicNativeSetObjectAttribute",
            ],
            "labels": [
                "Get Object Attribute",
                "Set Object Attribute",
            ],
            "separators": 1,
        },
        "Objects/Transformation": {
            "nodes": [
                "LogicNativeApplyMovement",
                "LogicNativeApplyRotation",
                "LogicNativeApplyForce",
                "LogicNativeApplyForceToTarget",
                "LogicNativeApplyTorque",
                "LogicNativeApplyImpulse",
                "LogicNativeMoveToward",
                "LogicNativeNavigate",
                "LogicNativeFollowPath",
                "LogicNativeTranslate",
                "LogicNativeRotateToward",
                "LogicNativeSlowFollow",
                "LogicNativeAlignAxisToVector",
            ],
            "separators": 1,
        },
        "Objects/Object Data": {
            "nodes": [
                "LogicNativeFindObject",
                "LogicNativeObjectByName",
                "LogicNativeGetOwner",
                "LogicNativeGetObjectID",
                "LogicNativeGetAxisVector",
                "LogicNativeGetDistance",
                "LogicNativeGetAverageCenterPosition",
                "LogicNativePredictMotion",
                "LogicNativeAddObject",
                "LogicNativeSetParent",
                "LogicNativeGetParent",
                "LogicNativeGetChild",
                "LogicNativeGetChildByName",
                "LogicNativeRemoveParent",
                "LogicNativeRemoveObject",
                "LogicNativeReplaceMesh",
            ],
            "separators": 1,
        },
        "Objects/Curves": {
            "nodes": ["LogicNativeEvaluateCurve"],
        },
        "Scene": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Scene"]),
            "nodes": [
                "LogicNativeGetTimescale",
                "LogicNativeSetTimescale",
                "LogicNativeSetGamePaused",
                "LogicNativeLoadScene",
                "LogicNativeSetScene",
                "LogicNativeGetScene",
            ],
            "separators": 2,
        },
        "Scene/Camera": {
            "nodes": [
                "LogicNativeGetActiveCamera",
                "LogicNativeCameraRay",
                "LogicNativeSetCamera",
                "LogicNativeSetCameraFov",
                "LogicNativeSetCameraOrthoScale",
                "LogicNativeWorldToScreen",
                "LogicNativeScreenToWorld",
            ],
        },
        "Scene/Collections": {
            "nodes": [
                "LogicNativeGetCollection",
                "LogicNativeGetCollectionObjects",
                "LogicNativeGetCollectionObjectNames",
                "LogicNativeSetCollectionVisibility",
                "LogicNativeSetOverlayCollection",
                "LogicNativeRemoveOverlayCollection",
            ],
        },
        "Render": {
            "nodes": [
                "LogicNativeGetFullscreen",
                "LogicNativeSetFullscreen",
                "LogicNativeGetResolution",
                "LogicNativeSetResolution",
                "LogicNativeGetVSync",
                "LogicNativeSetVSync",
                "LogicNativeShowFramerate",
                "LogicNativeShowProfile",
                "LogicNativeDrawLine",
                "LogicNativeDrawCube",
                "LogicNativeDrawBox",
                "LogicNativeDraw",
            ],
            "separators": 2,
        },
        "Logic": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Logic"]),
            "nodes": [
                "LogicNativeBranch",
                "LogicNativeFlowGate",
                "LogicNativeAnyFlow",
                "LogicNativeSequence",
                "LogicNativeSwitchFlowInt",
                "LogicNativeSwitchFlowString",
                "LogicNativeFlowRouter",
                "LogicNativeFlipFlop",
                "LogicNativeRepeatLimit",
                "LogicNativeGate",
                "LogicNativeNone",
                "LogicNativeNotNone",
                "LogicNativeValueValid",
                "LogicNativeCollision",
                "LogicNativeObjectsColliding",
            ],
            "separators": 3,
        },
        "Logic/Trees": {
            "nodes": [
                "LogicNativeStartLogicTree",
                "LogicNativeStopLogicTree",
                "LogicNativeRunLogicTree",
                "LogicNativeInstallLogicTree",
                "LogicNativeLogicTreeStatus",
            ],
        },
        "Logic/Loops": {
            "nodes": [
                "LogicNativeLoop",
                "LogicNativeBreakLoop",
            ],
        },
        "Time": {
            "nodes": [
                "LogicNativeTime",
                "LogicNativeDeltaFactor",
                "LogicNativeOnNextFrame",
                "LogicNativeDelay",
                "LogicNativeTimer",
                "LogicNativeCooldown",
                "LogicNativePulsify",
                "LogicNativeBarrier",
                "LogicNativeTweenValue",
            ],
            "separators": 1,
        },
        "Math": {
            "nodes": [
                "LogicNativeMath",
                "LogicNativeFormula",
                "LogicNativeVectorMath",
                "LogicNativeClampValue",
                "LogicNativeCompare",
                "LogicNativeFloatCurve",
                "LogicNativeMapRange",
                "LogicNativeThreshold",
                "LogicNativeRangedThreshold",
                "LogicNativeWithinRange",
            ],
            "separators": 2,
        },
        "Physics": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Physics"]),
            "nodes": [
                "LogicNativeGetCollisionGroup",
                "LogicNativeSetCollisionGroup",
                "LogicNativeGetGravity",
                "LogicNativeSetGravity",
                "LogicNativeRaycast",
                "LogicNativeRaycastAll",
                "LogicNativeRaycastFromMesh",
                "LogicNativeShapeCast",
                "LogicNativeShapeCastAll",
                "LogicNativeProjectileRay",
                "LogicNativeAddPhysicsConstraint",
                "LogicNativeGetRigidBodyConstraints",
                "LogicNativeRemovePhysicsConstraint",
                "LogicNativeObjectsColliding",
                "LogicNativeSetPhysics",
                "LogicNativeSetPhysicsPaused",
                "LogicNativeSetDynamics",
                "LogicNativeRebuildCollisionShape",
                "LogicNativeGetRigidBodyAttribute",
                "LogicNativeSetRigidBodyAttribute",
            ],
            "separators": 2,
        },
        "Physics/Vehicle": {
            "nodes": [
                "LogicNativeVehicleControl",
                "LogicNativeVehicleAccelerate",
                "LogicNativeVehicleBrake",
                "LogicNativeVehicleSetAttributes",
                "LogicNativeVehicleSteer",
            ],
        },
        "Data": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Data"]),
        },
        "UI": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["UI"]),
        },
        "Layout": {
            "nodes": ["NodeReroute", "NodeFrame"],
        },
    }

    for menu_path, expectation in expected_menu_contents.items():
        require(
            menu_path in logic_add_menu_cls.pathing_dict,
            f"LogicNodeTree add menu path missing: {menu_path}",
        )
        menu_idname = logic_add_menu_cls.pathing_dict[menu_path]
        menu_cls = find_menu_class(logic_menu_module, menu_idname)
        require(menu_cls is not None, f"LogicNodeTree menu class missing for {menu_path}")
        menu_layout = draw_menu_layout(logic_menu_module, menu_cls, context)

        expected_paths = expectation.get("paths", [])
        expected_menus = [logic_add_menu_cls.pathing_dict[path] for path in expected_paths]
        require(
            menu_layout.menu_calls == expected_menus,
            f"LogicNodeTree menu {menu_path} has wrong submenu ordering: {menu_layout.menu_calls}",
        )

        expected_nodes = expectation.get("nodes", [])
        require(
            operator_types(menu_layout) == expected_nodes,
            f"LogicNodeTree menu {menu_path} has wrong node entries: {operator_types(menu_layout)}",
        )

        expected_labels = expectation.get("labels")
        if expected_labels is not None:
            actual_labels = [operator_call.text for operator_call in menu_layout.operator_calls]
            require(
                actual_labels == expected_labels,
                f"LogicNodeTree menu {menu_path} has wrong node labels: {actual_labels}",
            )

        expected_separators = expectation.get("separators", 0)
        require(
            menu_layout.separator_count == expected_separators,
            f"LogicNodeTree menu {menu_path} has wrong separator count: {menu_layout.separator_count}",
        )

    visible_node_items = list(logic_menu_module.iter_visible_node_items())
    require(visible_node_items, "LogicNodeTree menu exposes no visible node items")

    visible_node_idnames = [item["idname"] for item in visible_node_items]
    visible_native_node_idnames = {
        node_idname for node_idname in visible_node_idnames if node_idname.startswith("LogicNative")
    }
    expected_native_node_idnames = set(logic_menu_module.catalog_native_idnames(
        status=logic_menu_module.LOGIC_NODE_STATUS_IMPLEMENTED,
        visible=True,
    ))
    require(
        visible_native_node_idnames == expected_native_node_idnames,
        f"LogicNodeTree menu exposes the wrong native node set: {sorted(visible_native_node_idnames)}",
    )
    deferred_visible_idnames = sorted(
        visible_native_node_idnames.intersection(logic_menu_module.deferred_native_idnames())
    )
    require(
        not deferred_visible_idnames,
        f"LogicNodeTree menu exposes deferred native node idnames: {deferred_visible_idnames}",
    )
    unsupported_compiler_idnames = unsupported_compiler_node_idnames()
    unsupported_visible_idnames = sorted(
        visible_native_node_idnames.intersection(unsupported_compiler_idnames)
    )
    require(
        not unsupported_visible_idnames,
        "LogicNodeTree menu exposes native nodes without runtime command handlers: "
        f"{unsupported_visible_idnames}",
    )
    unsupported_catalog_errors = []
    for node_idname in sorted(unsupported_compiler_idnames):
        entry = logic_menu_module.LOGIC_NODE_PARITY_CATALOG.get(node_idname)
        if entry is None:
            unsupported_catalog_errors.append(f"{node_idname}: missing parity catalog entry")
            continue
        if entry["status"] == logic_menu_module.LOGIC_NODE_STATUS_IMPLEMENTED:
            unsupported_catalog_errors.append(f"{node_idname}: marked implemented")
        if entry["visible"]:
            unsupported_catalog_errors.append(f"{node_idname}: catalog-visible")
    require(
        not unsupported_catalog_errors,
        "Native nodes routed to CompileUnsupportedCommandNode must stay hidden/deferred: "
        + "; ".join(unsupported_catalog_errors),
    )
    require(
        {"NodeFrame", "NodeReroute"}.issubset(set(visible_node_idnames)),
        f"LogicNodeTree layout nodes are missing from the visible menu set: {visible_node_idnames}",
    )

    for item in visible_node_items:
        if item["idname"].startswith("LogicNative"):
            require(
                item["idname"] in logic_menu_module.LOGIC_NODE_PARITY_CATALOG,
                f"Visible native node is missing from the parity catalog: {item['idname']}",
            )
        require(
            item["status"] == logic_menu_module.LOGIC_NODE_STATUS_IMPLEMENTED,
            f"Unexpected placeholder menu item became visible: {item}",
        )
        require(
            hasattr(bpy.types, item["idname"]),
            f"LogicNodeTree menu exposes an unregistered node idname: {item['idname']}",
        )

    for retired_menu_path in ("Objects/Get Attribute", "Objects/Set Attribute"):
        require(
            retired_menu_path not in logic_add_menu_cls.pathing_dict,
            f"Retired extra submenu is still registered: {retired_menu_path}",
        )

    expected_object_catalog_entries = {
        "LogicNativeGetObjectAttribute": ("Get Object Attribute", "Objects"),
        "LogicNativeSetObjectAttribute": ("Set Object Attribute", "Objects"),
        "LogicNativeGetObjectID": ("Get Object ID", "Objects/Object Data"),
        "LogicNativeGetAxisVector": ("Get Axis Vector", "Objects/Object Data"),
    }
    for node_idname, (expected_label, expected_path) in expected_object_catalog_entries.items():
        entry = logic_menu_module.LOGIC_NODE_PARITY_CATALOG[node_idname]
        require(
            visible_node_idnames.count(node_idname) == 1,
            f"{node_idname} must appear exactly once in the LogicNodeTree menu",
        )
        require(entry["label"] == expected_label, f"{node_idname} has the wrong catalog label")
        require(entry["menu_path"] == expected_path, f"{node_idname} has the wrong menu path")


def validate_catalog_runtime_parity():
    from bl_ui.node_add_menu_logic_catalog import (
        LOGIC_NODE_PARITY_CATALOG,
        LOGIC_NODE_STATUS_IMPLEMENTED,
    )

    repo_root = Path(__file__).resolve().parents[2]
    registry_path = repo_root / "source/gameengine/LogicNodes/intern/LN_NodeRegistry.cpp"
    descriptors_path = repo_root / "source/blender/nodes/NOD_logic_descriptors.hh"
    registry_text = registry_path.read_text(encoding="utf-8")
    descriptors_text = descriptors_path.read_text(encoding="utf-8")

    registered_idnames = set(re.findall(
        r'\{"(LogicNative[A-Za-z0-9_]+)",\s*LN_NodeKind::',
        registry_text,
    ))
    catalog_idnames = set(LOGIC_NODE_PARITY_CATALOG)
    untracked_registry_idnames = sorted(registered_idnames - catalog_idnames)
    if untracked_registry_idnames:
        raise RuntimeError(
            "Registered native nodes missing from the parity catalog: "
            + ", ".join(untracked_registry_idnames)
        )

    missing_registry = []
    missing_descriptors = []
    missing_rna = []
    for entry in LOGIC_NODE_PARITY_CATALOG.values():
        if entry.get("status") != LOGIC_NODE_STATUS_IMPLEMENTED:
            continue
        idname = entry["idname"]
        if f'"{idname}"' not in registry_text:
            missing_registry.append(idname)
        if f'"{idname}"' not in descriptors_text:
            missing_descriptors.append(idname)
        if not hasattr(bpy.types, idname):
            missing_rna.append(idname)

    if missing_registry:
        raise RuntimeError(
            "Catalog implemented nodes missing from LN_NodeRegistry.cpp: "
            + ", ".join(sorted(missing_registry))
        )
    if missing_descriptors:
        raise RuntimeError(
            "Catalog implemented nodes missing from NOD_logic_descriptors.hh: "
            + ", ".join(sorted(missing_descriptors))
        )
    if missing_rna:
        raise RuntimeError(
            "Catalog implemented nodes missing from RNA registration: "
            + ", ".join(sorted(missing_rna))
        )


def validate_logic_tree_validation_operator():
    tree = new_logic_tree("LN Validation Operator Probe")
    try:
        result = bpy.ops.node.validate_logic_tree(tree_name=tree.name)
        require(
            result == {"FINISHED"},
            f"Validate Logic Tree should accept an empty LogicNodeTree, got {result}",
        )

        on_update = tree.nodes.new("LogicNativeOnUpdate")
        unsupported = tree.nodes.new("LogicNativeLoadFileContent")
        tree.links.new(on_update.outputs["Out"], flow_input(unsupported))
        try:
            bpy.ops.node.validate_logic_tree(tree_name=tree.name)
        except RuntimeError as exc:
            require(
                "does not have a runtime command implementation" in str(exc),
                f"Validate Logic Tree raised an unexpected error: {exc}",
            )
        else:
            raise RuntimeError("Validate Logic Tree accepted an unsupported runtime handler")
    finally:
        bpy.data.node_groups.remove(tree)


def validate_catalog_execution_input_labels():
    from bl_ui.node_add_menu_logic_catalog import (
        LOGIC_NODE_PARITY_CATALOG,
        LOGIC_NODE_STATUS_IMPLEMENTED,
    )

    tree = bpy.data.node_groups.new("LN Execution Input Label Audit", "LogicNodeTree")
    errors = []
    try:
        for entry in LOGIC_NODE_PARITY_CATALOG.values():
            if entry.get("status") != LOGIC_NODE_STATUS_IMPLEMENTED:
                continue
            idname = entry["idname"]
            try:
                node = tree.nodes.new(idname)
            except RuntimeError as exc:
                errors.append(f"{idname}: failed to create node for execution-label audit: {exc}")
                continue

            for socket in node.inputs:
                if socket.bl_idname != LOGIC_EXECUTION_SOCKET_IDNAME:
                    continue
                if socket.identifier == "Condition" and socket.name != "Flow":
                    errors.append(
                        f"{idname}.{socket.identifier}: execution input should display Flow, "
                        f"got {socket.name!r}"
                    )
                if socket.name in {"Condition", "default_value"}:
                    errors.append(
                        f"{idname}.{socket.identifier}: execution input has obsolete visible "
                        f"label {socket.name!r}"
                    )
                if not socket.hide_value:
                    errors.append(
                        f"{idname}.{socket.identifier}: execution input should hide default "
                        "value UI"
                    )

            tree.nodes.remove(node)
    finally:
        bpy.data.node_groups.remove(tree)

    require(
        not errors,
        "LogicNative execution input label audit failed:\n" + "\n".join(errors),
    )


def validate_value_change_node_ui():
    tree = bpy.data.node_groups.new("LN Value Change UI Audit", "LogicNodeTree")
    try:
        changed = tree.nodes.new("LogicNativeValueChanged")
        changed_to = tree.nodes.new("LogicNativeValueChangedTo")

        require(
            "ignore_initial_none" in changed.bl_rna.properties,
            "On Value Changed is missing the Ignore Initial None option",
        )
        require(
            changed.ignore_initial_none is False,
            "On Value Changed should observe None-to-value transitions by default",
        )
        changed.ignore_initial_none = True
        require(
            changed.ignore_initial_none is True,
            "On Value Changed Ignore Initial None option is not writable",
        )

        target = next(
            (socket for socket in changed_to.inputs if socket.identifier == "Target"),
            None,
        )
        require(target is not None, "On Value Changed To is missing its Target input")
        require(
            target.name == "Target",
            f"On Value Changed To Target input has an empty or incorrect label: {target.name!r}",
        )

        result = next(
            (socket for socket in changed_to.outputs if socket.identifier == "Result"),
            None,
        )
        require(result is not None, "On Value Changed To is missing its result output")
        require(
            result.name == "If Changed To",
            f"On Value Changed To result output has an unclear label: {result.name!r}",
        )
    finally:
        bpy.data.node_groups.remove(tree)


def validate_typecast_node_ui():
    tree = bpy.data.node_groups.new("LN Typecast UI Audit", "LogicNodeTree")
    try:
        node = tree.nodes.new("LogicNativeTypecast")
        require(node.to_type == "INT", "Typecast should preserve Integer as its legacy default")
        require(
            "integer_rounding" in node.bl_rna.properties,
            "Typecast is missing its Float to Integer conversion mode",
        )
        require(
            node.integer_rounding == "TRUNCATE",
            "Typecast should preserve truncation as its legacy Float to Integer behavior",
        )

        expected_outputs = {
            "INT": ("Integer", "NodeSocketLogicInt"),
            "BOOL": ("Boolean", "NodeSocketLogicBool"),
            "STRING": ("String", "NodeSocketLogicString"),
            "FLOAT": ("Float", "NodeSocketLogicFloat"),
            "VECTOR": ("Vector", "NodeSocketLogicVector"),
        }
        for target, (expected_identifier, expected_idname) in expected_outputs.items():
            node.to_type = target
            bpy.context.view_layer.update()
            available_outputs = [socket for socket in node.outputs if not socket.is_unavailable]
            require(
                len(available_outputs) == 1,
                f"Typecast {target} should expose exactly one output, got "
                f"{[socket.identifier for socket in available_outputs]}",
            )
            output = available_outputs[0]
            require(
                output.identifier == expected_identifier,
                f"Typecast {target} exposed {output.identifier!r} instead of "
                f"{expected_identifier!r}",
            )
            require(
                output.bl_idname == expected_idname,
                f"Typecast {target} output uses {output.bl_idname!r} instead of "
                f"{expected_idname!r}",
            )

        node.to_type = "INT"
        for mode in ("TRUNCATE", "ROUND", "FLOOR", "CEIL"):
            node.integer_rounding = mode
            require(
                node.integer_rounding == mode,
                f"Typecast Integer conversion mode {mode} is not writable",
            )
    finally:
        bpy.data.node_groups.remove(tree)


def validate_rotation_node_ui():
    tree = bpy.data.node_groups.new("LN Rotation Node UI Audit", "LogicNodeTree")
    try:
        combine = tree.nodes.new("LogicNativeEuler")
        separate = tree.nodes.new("LogicNativeSeparateEuler")

        require(combine.bl_label == "Combine Rotation", "Combine Rotation has an outdated label")
        require(
            separate.bl_label == "Separate Rotation",
            "Separate Rotation has an outdated label",
        )
        require(
            "XYZ Euler order" in combine.bl_rna.description,
            "Combine Rotation is missing its XYZ Euler-order description",
        )
        require(
            "angles may differ while preserving the same orientation"
            in separate.bl_rna.description,
            "Separate Rotation should explain equivalent Euler-angle representations",
        )
        require(
            [socket.identifier for socket in combine.inputs] == ["X", "Y", "Z"],
            "Combine Rotation must preserve its X, Y, and Z input identifiers",
        )
        require(
            [socket.identifier for socket in combine.outputs] == ["Rotation"],
            "Combine Rotation must expose one Rotation output",
        )
        require(
            combine.outputs["Rotation"].name == "Rotation",
            "Combine Rotation output has an inconsistent visible label",
        )
        require(
            [socket.identifier for socket in separate.inputs] == ["Rotation"],
            "Separate Rotation must preserve its Rotation input identifier",
        )
        require(
            [socket.identifier for socket in separate.outputs] == ["X", "Y", "Z"],
            "Separate Rotation must expose X, Y, and Z outputs",
        )

        angle_sockets = [*combine.inputs, *separate.outputs]
        for socket in angle_sockets:
            require(
                socket.bl_idname == "NodeSocketFloatAngle",
                f"{socket.identifier} should use Blender's angle-aware float socket",
            )
            require(
                socket.bl_rna.properties["default_value"].subtype == "ANGLE",
                f"{socket.identifier} should display degrees while storing radians",
            )
        combine.inputs["X"].default_value = math.radians(90.0)
        require(
            math.isclose(combine.inputs["X"].default_value, math.pi / 2.0, abs_tol=1.0e-6),
            "Combine Rotation angle storage must remain radians",
        )
    finally:
        bpy.data.node_groups.remove(tree)

    print("Rotation node UI checks passed")


def validate_object_attribute_node_ui():
    tree = new_logic_tree("LN Object Attribute UI Audit")
    try:
        get_attribute = tree.nodes.new("LogicNativeGetObjectAttribute")
        set_attribute = tree.nodes.new("LogicNativeSetObjectAttribute")
        get_id = tree.nodes.new("LogicNativeGetObjectID")
        get_axis = tree.nodes.new("LogicNativeGetAxisVector")

        expected_labels = {
            get_attribute: "Get Object Attribute",
            set_attribute: "Set Object Attribute",
            get_id: "Get Object ID",
            get_axis: "Get Axis Vector",
        }
        for node, expected_label in expected_labels.items():
            require(
                node.bl_label == expected_label,
                f"{node.bl_idname} has the wrong visible label: {node.bl_label}",
            )
    finally:
        bpy.data.node_groups.remove(tree)

    print("Object attribute node UI checks passed")


def validate_set_physics_state_node_ui():
    tree = new_logic_tree("LN Set Physics State UI Audit")
    try:
        node = tree.nodes.new("LogicNativeSetDynamics")
        expected_items = {
            "DYNAMIC": "Restore Previous Physics State",
            "STATIC_COLLIDER": "Static",
            "GHOST": "Ghost",
            "NO_COLLISION": "No Collision",
            "RESTORE_ORIGINAL": "Restore Original Physics Type",
            "DYNAMIC_TYPE": "Dynamic",
            "RIGID_BODY": "Rigid Body",
            "KINEMATIC": "Kinematic",
        }
        items = {
            item.identifier: item.name
            for item in node.bl_rna.properties["dynamics_mode"].enum_items
            if item.identifier
        }
        require(
            items == expected_items,
            f"Set Physics State modes changed unexpectedly: {items!r}",
        )
        require(
            node.bl_label == "Set Physics State",
            "Set Physics State node has an outdated label",
        )
        require(
            node.bl_rna.properties["dynamics_mode"].name == "Physics State",
            "Set Physics State dropdown has an inaccurate label",
        )
        require(
            node.dynamics_mode == "RESTORE_ORIGINAL",
            "New Set Physics State nodes should default to Restore Original Physics Type",
        )

        enabled = next(socket for socket in node.inputs if socket.identifier == "Enabled")
        require(enabled.is_unavailable, "Enabled should be hidden outside Ghost mode")
        node.dynamics_mode = "GHOST"
        bpy.context.view_layer.update()
        require(not enabled.is_unavailable, "Ghost mode should expose Enabled")
        node.dynamics_mode = "KINEMATIC"
        bpy.context.view_layer.update()
        require(enabled.is_unavailable, "Kinematic mode should hide Enabled")
    finally:
        bpy.data.node_groups.remove(tree)

    print("Set Physics State node UI checks passed")


def validate_logic_socket_palette():
    for socket_idname, expected_color in LOGIC_SOCKET_COLORS.items():
        socket_type = getattr(bpy.types, socket_idname, None)
        require(socket_type is not None, f"Logic socket RNA type is missing: {socket_idname}")

    class LogicSocketPaletteProbeNode(bpy.types.Node):
        bl_idname = "LogicSocketPaletteProbeNode"
        bl_label = "Logic Socket Palette Probe"

        @classmethod
        def poll(cls, ntree):
            return ntree.bl_idname == "LogicNodeTree"

        def init(self, context):
            for socket_idname in LOGIC_SOCKET_COLORS:
                self.inputs.new(socket_idname, socket_idname)

    probe_tree = None
    bpy.utils.register_class(LogicSocketPaletteProbeNode)
    try:
        probe_tree = bpy.data.node_groups.new("LN Socket Palette Probe", "LogicNodeTree")
        probe_node = probe_tree.nodes.new(LogicSocketPaletteProbeNode.bl_idname)
        actual_socket_colors = {
            socket.bl_idname: socket.draw_color_simple() for socket in probe_node.inputs
        }
        execution_socket = probe_node.inputs[LOGIC_EXECUTION_SOCKET_IDNAME]
        require(
            not hasattr(execution_socket, "default_value"),
            "Logic execution sockets must not expose a boolean default_value",
        )
        for socket_idname, expected_color in LOGIC_SOCKET_COLORS.items():
            require(
                socket_idname in actual_socket_colors,
                f"Logic socket probe did not create socket: {socket_idname}",
            )
            require_color_close(actual_socket_colors[socket_idname], expected_color, socket_idname)
    finally:
        if probe_tree is not None:
            bpy.data.node_groups.remove(probe_tree)
        bpy.utils.unregister_class(LogicSocketPaletteProbeNode)


def validate_logic_math_operations():
    operation_items = bpy.types.LogicNativeMath.bl_rna.properties["operation"].enum_items
    operations = {item.identifier for item in operation_items}
    require(
        operations == EXPECTED_LOGIC_MATH_OPERATIONS,
        f"LogicNativeMath exposes the wrong operation set: {sorted(operations)}",
    )

    vector_operation_items = bpy.types.LogicNativeVectorMath.bl_rna.properties[
        "operation"].enum_items
    vector_operations = {item.identifier for item in vector_operation_items}
    require(
        vector_operations == EXPECTED_LOGIC_VECTOR_MATH_OPERATIONS,
        f"LogicNativeVectorMath exposes the wrong operation set: {sorted(vector_operations)}",
    )

    string_operation_items = bpy.types.LogicNativeStringOperation.bl_rna.properties[
        "operation"].enum_items
    string_operations = {item.identifier for item in string_operation_items}
    require(
        string_operations == EXPECTED_LOGIC_STRING_OPERATION_OPERATIONS,
        f"LogicNativeStringOperation exposes the wrong operation set: {sorted(string_operations)}",
    )

    vsync_items = bpy.types.LogicNativeSetVSync.bl_rna.properties["vsync_mode"].enum_items
    vsync_modes = {item.identifier for item in vsync_items}
    require(
        vsync_modes == EXPECTED_LOGIC_VSYNC_MODES,
        f"LogicNativeSetVSync exposes the wrong mode set: {sorted(vsync_modes)}",
    )

    vehicle_axis_items = bpy.types.LogicNativeVehicleAccelerate.bl_rna.properties[
        "vehicle_axis"].enum_items
    vehicle_axes = {item.identifier for item in vehicle_axis_items}
    require(
        vehicle_axes == EXPECTED_LOGIC_VEHICLE_AXES,
        f"LogicNativeVehicleAccelerate exposes the wrong vehicle axis set: {sorted(vehicle_axes)}",
    )

    constraint_break_match_items = bpy.types.LogicNativeOnConstraintBreak.bl_rna.properties[
        "match_mode"].enum_items
    constraint_break_match_modes = {
        item.identifier for item in constraint_break_match_items
    }
    require(
        constraint_break_match_modes == EXPECTED_LOGIC_CONSTRAINT_BREAK_MATCH_MODES,
        "LogicNativeOnConstraintBreak exposes the wrong match mode set: "
        f"{sorted(constraint_break_match_modes)}",
    )

    require(
        "use_local_space" in bpy.types.LogicNativeGetCharacterInfo.bl_rna.properties,
        "LogicNativeGetCharacterInfo is missing its local-space RNA property",
    )
    require(
        "use_local_space" in bpy.types.LogicNativeSetCharacterWalkDirection.bl_rna.properties,
        "LogicNativeSetCharacterWalkDirection is missing its local-space RNA property",
    )
    require(
        "use_local_space" in bpy.types.LogicNativeSetCharacterVelocity.bl_rna.properties,
        "LogicNativeSetCharacterVelocity is missing its local-space RNA property",
    )


def find_game_property(obj, name):
    index = obj.game.properties.find(name)
    if index == -1:
        return None
    return obj.game.properties[index]


def set_active_object(obj):
    for scene_obj in bpy.context.scene.objects:
        scene_obj.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def get_or_create_cube(name):
    obj = bpy.data.objects.get(name)
    if obj is not None:
        return obj

    bpy.ops.mesh.primitive_cube_add()
    obj = bpy.context.object
    obj.name = name
    return obj


def build_binding_probe(linked_tree):
    binding_object = get_or_create_cube(LOGIC_NODES_BINDING_OBJECT)
    set_active_object(binding_object)
    users_before_binding = linked_tree.users
    result = bpy.ops.object.logic_nodes_binding_add()
    require(result == {"FINISHED"}, f"Failed to add Logic Nodes binding: {result}")
    require(
        len(binding_object.game.logic_node_bindings) == 1,
        "Logic Nodes binding collection entry missing",
    )

    binding = binding_object.game.logic_node_bindings[0]
    binding.tree = linked_tree
    binding.enabled = True
    require(binding.tree_name == linked_tree.name, "Binding tree name mismatch")
    require(
        linked_tree.users == users_before_binding + 1,
        "Logic Nodes binding did not register as a real node-tree user",
    )

    original_tree_name = linked_tree.name
    linked_tree.name = f"{original_tree_name} Rename Probe"
    require(binding.tree == linked_tree, "Binding lost its tree pointer after a datablock rename")
    require(
        binding.tree_name == linked_tree.name,
        "Binding did not expose the renamed tree name immediately",
    )
    linked_tree.name = original_tree_name
    require(
        binding.tree_name == original_tree_name,
        "Binding did not follow a second node-tree rename",
    )

    users_before_copy = linked_tree.users
    binding_object_copy = binding_object.copy()
    binding_object_copy.name = f"{LOGIC_NODES_BINDING_OBJECT} Copy"
    bpy.context.scene.collection.objects.link(binding_object_copy)
    require(
        len(binding_object_copy.game.logic_node_bindings) == 1
        and binding_object_copy.game.logic_node_bindings[0].tree == linked_tree,
        "Copying an object did not preserve its Logic Nodes tree binding",
    )
    require(
        linked_tree.users == users_before_copy + 1,
        "Copying a bound object did not register another real node-tree user",
    )
    bpy.data.objects.remove(binding_object_copy, do_unlink=True)
    require(
        linked_tree.users == users_before_copy,
        "Removing a copied bound object did not release its node-tree user",
    )

    clear_object = get_or_create_cube(LOGIC_NODES_CLEAR_OBJECT)
    set_active_object(clear_object)
    result = bpy.ops.object.logic_nodes_binding_add()
    require(result == {"FINISHED"}, f"Failed to add clear-test binding: {result}")
    clear_object.game.logic_node_bindings[0].tree = linked_tree
    users_before_clear = linked_tree.users
    result = bpy.ops.object.logic_nodes_binding_clear()
    require(result == {"FINISHED"}, f"Failed to clear Logic Nodes binding: {result}")
    require(
        len(clear_object.game.logic_node_bindings) == 0,
        "Logic Nodes binding clear left collection entries behind",
    )
    require(
        linked_tree.users == users_before_clear - 1,
        "Clearing Logic Nodes bindings did not release the node-tree user",
    )


def validate_binding_remap_safety():
    tree_name = "LN Binding Remap Probe"
    tree = bpy.data.node_groups.new(tree_name, "LogicNodeTree")
    obj = get_or_create_cube(tree_name)
    binding = obj.game.logic_node_binding_new()
    binding.tree = tree

    bpy.data.node_groups.remove(tree, do_unlink=True)
    require(binding.tree is None, "Deleting a bound node tree left a dangling binding pointer")
    require(
        binding.tree_name == "",
        "Deleting a bound node tree left a stale public binding name",
    )

    replacement = bpy.data.node_groups.new(tree_name, "LogicNodeTree")
    require(
        binding.tree is None,
        "Creating a same-named tree unexpectedly reconnected a deleted binding",
    )

    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.node_groups.remove(replacement)


def disable_legacy_logic_addons():
    """Keep Python logic addons off during native LogicNodeTree tests (separate product path)."""
    for module_name in ("bge_netlogic", "bge_bricknodes"):
        try:
            addon_utils.disable(module_name, default_set=True)
        except Exception:
            pass

    blocked_prefixes = ("bge_netlogic", "bge_bricknodes")
    blocked_names = {"_on_deps_update", "establish_synching", "update_all_trees"}
    for handlers in (
        bpy.app.handlers.depsgraph_update_post,
        bpy.app.handlers.game_post,
        bpy.app.handlers.load_post,
        bpy.app.handlers.undo_post,
    ):
        for handler in list(handlers):
            module_name = getattr(handler, "__module__", "")
            function_name = getattr(handler, "__name__", "")
            if module_name.startswith(blocked_prefixes) or function_name in blocked_names:
                handlers.remove(handler)


def build_probe_trees():
    global EXPECTED_LINKED_NODE_COUNT, EXPECTED_LINKED_LINK_COUNT

    require(hasattr(bpy.types, "LogicNodeTree"), "LogicNodeTree RNA type is not registered")

    empty_tree = new_logic_tree("LN Empty Tree")
    require(empty_tree.bl_idname == "LogicNodeTree", "Empty tree has wrong RNA type")

    single_node_tree = new_logic_tree("LN Single Node Tree")
    init_node = single_node_tree.nodes.new("LogicNativeOnInit")
    init_node.name = "On Init Probe"

    linked_tree = new_logic_tree("LN Linked Tree")
    on_update = linked_tree.nodes.new("LogicNativeOnUpdate")
    on_fixed_update = linked_tree.nodes.new("LogicNativeOnFixedUpdate")
    branch = linked_tree.nodes.new("LogicNativeBranch")
    flow_gate = linked_tree.nodes.new("LogicNativeFlowGate")
    any_flow = linked_tree.nodes.new("LogicNativeAnyFlow")
    sequence = linked_tree.nodes.new("LogicNativeSequence")
    gate = linked_tree.nodes.new("LogicNativeGate")
    condition = linked_tree.nodes.new("LogicNativeValueBool")
    value_a = linked_tree.nodes.new("LogicNativeValueFloat")
    value_b = linked_tree.nodes.new("LogicNativeValueFloat")
    value_int = linked_tree.nodes.new("LogicNativeValueInt")
    value_string = linked_tree.nodes.new("LogicNativeValueString")
    string_operation = linked_tree.nodes.new("LogicNativeStringOperation")
    formatted_string = linked_tree.nodes.new("LogicNativeFormattedString")
    value_color = linked_tree.nodes.new("LogicNativeValueColor")
    color_rgb = linked_tree.nodes.new("LogicNativeColorRGB")
    color_rgba = linked_tree.nodes.new("LogicNativeColorRGBA")
    euler = linked_tree.nodes.new("LogicNativeEuler")
    combine_xy = linked_tree.nodes.new("LogicNativeCombineXY")
    combine_xyz = linked_tree.nodes.new("LogicNativeCombineXYZ")
    separate_xy = linked_tree.nodes.new("LogicNativeSeparateXY")
    separate_xyz = linked_tree.nodes.new("LogicNativeSeparateXYZ")
    invert_value = linked_tree.nodes.new("LogicNativeInvertValue")
    clamp_value = linked_tree.nodes.new("LogicNativeClampValue")
    compare = linked_tree.nodes.new("LogicNativeCompare")
    map_range = linked_tree.nodes.new("LogicNativeMapRange")
    float_curve = linked_tree.nodes.new("LogicNativeFloatCurve")
    threshold = linked_tree.nodes.new("LogicNativeThreshold")
    ranged_threshold = linked_tree.nodes.new("LogicNativeRangedThreshold")
    within_range = linked_tree.nodes.new("LogicNativeWithinRange")
    math = linked_tree.nodes.new("LogicNativeMath")
    target_position = linked_tree.nodes.new("LogicNativeValueVector")
    get_position = new_get_object_attribute(linked_tree, "WORLD_POSITION")
    get_local_position = new_get_object_attribute(linked_tree, "LOCAL_POSITION")
    get_world_orientation = new_get_object_attribute(linked_tree, "WORLD_ORIENTATION")
    get_local_orientation = new_get_object_attribute(linked_tree, "LOCAL_ORIENTATION")
    get_world_scale = new_get_object_attribute(linked_tree, "WORLD_SCALE")
    get_local_scale = new_get_object_attribute(linked_tree, "LOCAL_SCALE")
    get_visibility = new_get_object_attribute(linked_tree, "VISIBLE")
    get_object_color = new_get_object_attribute(linked_tree, "COLOR")
    get_light_color = linked_tree.nodes.new("LogicNativeGetLightColor")
    get_light_power = linked_tree.nodes.new("LogicNativeGetLightPower")
    get_velocity = new_get_object_attribute(linked_tree, "WORLD_LINEAR_VELOCITY")
    get_local_velocity = new_get_object_attribute(linked_tree, "LOCAL_LINEAR_VELOCITY")
    get_angular_velocity = new_get_object_attribute(linked_tree, "WORLD_ANGULAR_VELOCITY")
    get_local_angular_velocity = new_get_object_attribute(linked_tree, "LOCAL_ANGULAR_VELOCITY")
    get_name = new_get_object_attribute(linked_tree, "NAME")
    get_world_transform = new_get_object_attribute(linked_tree, "WORLD_TRANSFORM")
    get_local_transform = new_get_object_attribute(linked_tree, "LOCAL_TRANSFORM")
    get_prop_float = linked_tree.nodes.new("LogicNativeGetGamePropertyFloat")
    get_prop_bool = linked_tree.nodes.new("LogicNativeGetGamePropertyBool")
    get_prop_int = linked_tree.nodes.new("LogicNativeGetGamePropertyInt")
    get_prop_string = linked_tree.nodes.new("LogicNativeGetGamePropertyString")
    vector_math = linked_tree.nodes.new("LogicNativeVectorMath")
    set_position = new_set_object_attribute(linked_tree, "WORLD_POSITION")
    set_local_position = new_set_object_attribute(linked_tree, "LOCAL_POSITION")
    set_world_orientation = new_set_object_attribute(linked_tree, "WORLD_ORIENTATION")
    set_local_orientation = new_set_object_attribute(linked_tree, "LOCAL_ORIENTATION")
    set_world_scale = new_set_object_attribute(linked_tree, "WORLD_SCALE")
    set_local_scale = new_set_object_attribute(linked_tree, "LOCAL_SCALE")
    set_visibility = new_set_object_attribute(linked_tree, "VISIBLE")
    set_object_color = new_set_object_attribute(linked_tree, "COLOR")
    set_light_color = linked_tree.nodes.new("LogicNativeSetLightColor")
    set_light_power = linked_tree.nodes.new("LogicNativeSetLightPower")
    set_light_shadow = linked_tree.nodes.new("LogicNativeSetLightShadow")
    make_light_unique = linked_tree.nodes.new("LogicNativeMakeLightUnique")
    set_velocity = new_set_object_attribute(linked_tree, "WORLD_LINEAR_VELOCITY")
    set_local_velocity = new_set_object_attribute(linked_tree, "LOCAL_LINEAR_VELOCITY")
    set_angular_velocity = new_set_object_attribute(linked_tree, "WORLD_ANGULAR_VELOCITY")
    set_local_angular_velocity = new_set_object_attribute(linked_tree, "LOCAL_ANGULAR_VELOCITY")
    set_world_transform = new_set_object_attribute(linked_tree, "WORLD_TRANSFORM")
    set_local_transform = new_set_object_attribute(linked_tree, "LOCAL_TRANSFORM")
    apply_movement = linked_tree.nodes.new("LogicNativeApplyMovement")
    apply_rotation = linked_tree.nodes.new("LogicNativeApplyRotation")
    apply_force = linked_tree.nodes.new("LogicNativeApplyForce")
    apply_torque = linked_tree.nodes.new("LogicNativeApplyTorque")
    apply_impulse = linked_tree.nodes.new("LogicNativeApplyImpulse")
    set_prop_float = linked_tree.nodes.new("LogicNativeSetGamePropertyFloat")
    set_prop_bool = linked_tree.nodes.new("LogicNativeSetGamePropertyBool")
    set_prop_int = linked_tree.nodes.new("LogicNativeSetGamePropertyInt")
    set_prop_string = linked_tree.nodes.new("LogicNativeSetGamePropertyString")
    on_next_frame = linked_tree.nodes.new("LogicNativeOnNextFrame")
    once = linked_tree.nodes.new("LogicNativeOnce")
    boolean_edge = linked_tree.nodes.new("LogicNativeBooleanEdge")
    value_changed = linked_tree.nodes.new("LogicNativeValueChanged")
    value_changed_to = linked_tree.nodes.new("LogicNativeValueChangedTo")
    value_changed.ignore_initial_none = True
    delay = linked_tree.nodes.new("LogicNativeDelay")
    timer = linked_tree.nodes.new("LogicNativeTimer")
    cooldown = linked_tree.nodes.new("LogicNativeCooldown")
    pulsify = linked_tree.nodes.new("LogicNativePulsify")
    barrier = linked_tree.nodes.new("LogicNativeBarrier")
    limit_range = linked_tree.nodes.new("LogicNativeLimitRange")
    random_value = linked_tree.nodes.new("LogicNativeRandomValue")
    value_switch = linked_tree.nodes.new("LogicNativeValueSwitch")
    store_value = linked_tree.nodes.new("LogicNativeStoreValue")
    has_property = linked_tree.nodes.new("LogicNativeHasProperty")
    toggle_property = linked_tree.nodes.new("LogicNativeToggleProperty")
    modify_property = linked_tree.nodes.new("LogicNativeModifyProperty")
    keyboard_key = linked_tree.nodes.new("LogicNativeKeyboardKey")
    mouse_button = linked_tree.nodes.new("LogicNativeMouseButton")
    gamepad_button = linked_tree.nodes.new("LogicNativeGamepadButton")
    input_action = linked_tree.nodes.new("LogicNativeInputAction")
    get_input_binding = linked_tree.nodes.new("LogicNativeGetInputBinding")
    rebind_input_action = linked_tree.nodes.new("LogicNativeRebindInputAction")
    input_map = linked_tree.nodes.new("LogicNativeInputMap")
    add_object = linked_tree.nodes.new("LogicNativeAddObject")
    set_parent = linked_tree.nodes.new("LogicNativeSetParent")
    remove_parent = linked_tree.nodes.new("LogicNativeRemoveParent")
    remove_object = linked_tree.nodes.new("LogicNativeRemoveObject")
    raycast_from_mesh = linked_tree.nodes.new("LogicNativeRaycastFromMesh")
    raycast_from_mesh.mesh_domain = "FACES"
    raycast_from_mesh.ray_direction = "INVERTED_NORMALS"
    raycast_from_mesh.inputs["Start Offset"].default_value = 0.125
    raycast_from_mesh.inputs["Max Distance"].default_value = 42.0
    raycast_from_mesh.inputs["Max Rays"].default_value = 17
    get_active_camera = linked_tree.nodes.new("LogicNativeGetActiveCamera")
    set_camera = linked_tree.nodes.new("LogicNativeSetCamera")
    set_camera_fov = linked_tree.nodes.new("LogicNativeSetCameraFov")
    set_camera_ortho_scale = linked_tree.nodes.new("LogicNativeSetCameraOrthoScale")
    world_to_screen = linked_tree.nodes.new("LogicNativeWorldToScreen")
    screen_to_world = linked_tree.nodes.new("LogicNativeScreenToWorld")
    get_fullscreen = linked_tree.nodes.new("LogicNativeGetFullscreen")
    set_fullscreen = linked_tree.nodes.new("LogicNativeSetFullscreen")
    get_resolution = linked_tree.nodes.new("LogicNativeGetResolution")
    set_resolution = linked_tree.nodes.new("LogicNativeSetResolution")
    get_vsync = linked_tree.nodes.new("LogicNativeGetVSync")
    set_vsync = linked_tree.nodes.new("LogicNativeSetVSync")
    show_framerate = linked_tree.nodes.new("LogicNativeShowFramerate")
    show_profile = linked_tree.nodes.new("LogicNativeShowProfile")
    get_character_info = linked_tree.nodes.new("LogicNativeGetCharacterInfo")
    character_jump = linked_tree.nodes.new("LogicNativeCharacterJump")
    set_character_gravity = linked_tree.nodes.new("LogicNativeSetCharacterGravity")
    set_character_jump_speed = linked_tree.nodes.new("LogicNativeSetCharacterJumpSpeed")
    set_character_max_jumps = linked_tree.nodes.new("LogicNativeSetCharacterMaxJumps")
    set_character_walk_direction = linked_tree.nodes.new("LogicNativeSetCharacterWalkDirection")
    set_character_velocity = linked_tree.nodes.new("LogicNativeSetCharacterVelocity")
    vehicle_control = linked_tree.nodes.new("LogicNativeVehicleControl")
    vehicle_accelerate = linked_tree.nodes.new("LogicNativeVehicleAccelerate")
    vehicle_brake = linked_tree.nodes.new("LogicNativeVehicleBrake")
    vehicle_steer = linked_tree.nodes.new("LogicNativeVehicleSteer")
    vehicle_set_attributes = linked_tree.nodes.new("LogicNativeVehicleSetAttributes")

    on_update.location = (-480.0, 120.0)
    on_fixed_update.location = (-480.0, 280.0)
    condition.location = (-480.0, -80.0)
    gate.location = (-350.0, -20.0)
    branch.location = (-220.0, 40.0)
    value_a.location = (-220.0, -180.0)
    value_b.location = (-220.0, -340.0)
    value_int.location = (-220.0, -500.0)
    value_string.location = (-220.0, -660.0)
    string_operation.location = (-220.0, -820.0)
    formatted_string.location = (-220.0, -980.0)
    value_color.location = (-220.0, -1140.0)
    color_rgb.location = (-220.0, -1300.0)
    color_rgba.location = (-220.0, -1460.0)
    euler.location = (-220.0, -1620.0)
    combine_xy.location = (-220.0, -1780.0)
    combine_xyz.location = (-220.0, -1940.0)
    separate_xy.location = (-220.0, -2100.0)
    separate_xyz.location = (-220.0, -2260.0)
    invert_value.location = (-220.0, -2420.0)
    clamp_value.location = (-220.0, -2580.0)
    compare.location = (-220.0, -2740.0)
    map_range.location = (-220.0, -2900.0)
    float_curve.location = (-480.0, -2900.0)
    threshold.location = (-220.0, -3060.0)
    ranged_threshold.location = (-220.0, -3220.0)
    within_range.location = (-220.0, -3380.0)
    math.location = (40.0, -260.0)
    target_position.location = (40.0, 20.0)
    get_position.location = (40.0, 180.0)
    get_local_position.location = (40.0, -80.0)
    get_world_orientation.location = (40.0, -160.0)
    get_local_orientation.location = (40.0, -240.0)
    get_world_scale.location = (40.0, -160.0)
    get_local_scale.location = (40.0, -240.0)
    get_visibility.location = (40.0, -320.0)
    get_object_color.location = (40.0, -400.0)
    get_velocity.location = (40.0, -240.0)
    get_local_velocity.location = (40.0, -400.0)
    get_angular_velocity.location = (40.0, -560.0)
    get_local_angular_velocity.location = (40.0, -720.0)
    get_name.location = (40.0, -880.0)
    get_world_transform.location = (40.0, -1040.0)
    get_local_transform.location = (40.0, -1200.0)
    get_prop_float.location = (40.0, -460.0)
    get_prop_bool.location = (40.0, -620.0)
    get_prop_int.location = (40.0, -780.0)
    get_prop_string.location = (40.0, -940.0)
    vector_math.location = (300.0, 140.0)
    set_position.location = (560.0, 120.0)
    set_local_position.location = (560.0, -60.0)
    set_world_orientation.location = (560.0, -240.0)
    set_local_orientation.location = (560.0, -420.0)
    set_world_scale.location = (560.0, -240.0)
    set_local_scale.location = (560.0, -420.0)
    set_visibility.location = (560.0, -600.0)
    set_object_color.location = (560.0, -780.0)
    set_velocity.location = (300.0, -240.0)
    set_local_velocity.location = (300.0, -400.0)
    set_angular_velocity.location = (300.0, -560.0)
    set_local_angular_velocity.location = (300.0, -720.0)
    set_world_transform.location = (300.0, -800.0)
    set_local_transform.location = (300.0, -960.0)
    apply_movement.location = (300.0, -880.0)
    apply_rotation.location = (300.0, -1040.0)
    apply_force.location = (300.0, -1200.0)
    apply_torque.location = (300.0, -1360.0)
    apply_impulse.location = (300.0, -280.0)
    set_prop_float.location = (300.0, -460.0)
    set_prop_bool.location = (300.0, -620.0)
    set_prop_int.location = (300.0, -780.0)
    set_prop_string.location = (300.0, -940.0)
    on_next_frame.location = (820.0, 120.0)
    any_flow.location = (820.0, -40.0)
    once.location = (820.0, -200.0)
    value_changed.location = (820.0, -360.0)
    value_changed_to.location = (820.0, -520.0)
    delay.location = (820.0, -680.0)
    timer.location = (820.0, -840.0)
    pulsify.location = (820.0, -1000.0)
    barrier.location = (820.0, -1160.0)
    limit_range.location = (1080.0, 120.0)
    random_value.location = (1080.0, -40.0)
    value_switch.location = (1080.0, -200.0)
    store_value.location = (1080.0, -360.0)
    has_property.location = (1080.0, -520.0)
    toggle_property.location = (1080.0, -680.0)
    modify_property.location = (1080.0, -840.0)
    keyboard_key.location = (1340.0, 120.0)
    mouse_button.location = (1340.0, -40.0)
    gamepad_button.location = (1340.0, -200.0)
    add_object.location = (1340.0, -360.0)
    set_parent.location = (1340.0, -520.0)
    remove_parent.location = (1340.0, -680.0)
    remove_object.location = (1340.0, -840.0)
    get_active_camera.location = (1600.0, 120.0)
    set_camera.location = (1600.0, -40.0)
    set_camera_fov.location = (1600.0, -200.0)
    set_camera_ortho_scale.location = (1600.0, -360.0)
    world_to_screen.location = (1600.0, -520.0)
    screen_to_world.location = (1600.0, -680.0)
    get_fullscreen.location = (1860.0, 120.0)
    set_fullscreen.location = (1860.0, -40.0)
    get_resolution.location = (1860.0, -200.0)
    set_resolution.location = (1860.0, -360.0)
    get_vsync.location = (1860.0, -520.0)
    set_vsync.location = (1860.0, -680.0)
    show_framerate.location = (1860.0, -840.0)
    show_profile.location = (1860.0, -1000.0)
    get_character_info.location = (2120.0, 120.0)
    character_jump.location = (2120.0, -40.0)
    set_character_gravity.location = (2120.0, -200.0)
    set_character_jump_speed.location = (2120.0, -360.0)
    set_character_max_jumps.location = (2120.0, -520.0)
    set_character_walk_direction.location = (2120.0, -680.0)
    set_character_velocity.location = (2120.0, -840.0)
    vehicle_control.location = (2380.0, 280.0)
    vehicle_accelerate.location = (2380.0, 120.0)
    vehicle_brake.location = (2380.0, -40.0)
    vehicle_steer.location = (2380.0, -200.0)
    vehicle_set_attributes.location = (2380.0, -360.0)

    value_output(condition).default_value = True
    gate.operation = "AND"
    value_output(value_a).default_value = 2.0
    value_output(value_b).default_value = 3.0
    value_output(value_int).default_value = 7
    value_output(value_string).default_value = "running"
    string_operation.operation = "REPLACE"
    string_operation.inputs["String"].default_value = "hello_world"
    string_operation.inputs["Substring"].default_value = "_"
    string_operation.inputs["Replacement"].default_value = " "
    formatted_string.inputs["Format String"].default_value = "A is {} and B is {}"
    formatted_string.inputs["A"].default_value = "Hello"
    formatted_string.inputs["B"].default_value = "World"
    value_output(value_color).default_value = (0.25, 0.5, 0.75, 1.0)
    color_rgb.inputs["R"].default_value = 0.1
    color_rgb.inputs["G"].default_value = 0.2
    color_rgb.inputs["B"].default_value = 0.3
    color_rgba.inputs["R"].default_value = 0.25
    color_rgba.inputs["G"].default_value = 0.5
    color_rgba.inputs["B"].default_value = 0.75
    color_rgba.inputs["A"].default_value = 1.0
    euler.inputs["X"].default_value = 0.1
    euler.inputs["Y"].default_value = 0.2
    euler.inputs["Z"].default_value = 0.3
    combine_xy.inputs["X"].default_value = 1.0
    combine_xy.inputs["Y"].default_value = 2.0
    combine_xyz.inputs["X"].default_value = 1.0
    combine_xyz.inputs["Y"].default_value = 2.0
    combine_xyz.inputs["Z"].default_value = 3.0
    math.operation = "MAXIMUM"
    invert_value.inputs["Value"].default_value = 4.0
    clamp_value.inputs["Value"].default_value = 2.0
    clamp_value.inputs["Min"].default_value = -1.0
    clamp_value.inputs["Max"].default_value = 1.0
    compare.operation = "GREATER_THAN"
    compare.inputs["A"].default_value = 2.0
    compare.inputs["B"].default_value = 1.0
    map_range.use_clamp = True
    map_range.inputs["Value"].default_value = 0.5
    map_range.inputs["From Min"].default_value = 0.0
    map_range.inputs["From Max"].default_value = 1.0
    map_range.inputs["To Min"].default_value = -10.0
    map_range.inputs["To Max"].default_value = 10.0
    float_curve.inputs["Factor"].default_value = 0.4
    float_curve.inputs["Value"].default_value = 0.25
    float_curve.mapping.extend = "HORIZONTAL"
    float_curve.mapping.use_clip = False
    float_curve.mapping.curves[0].points.new(0.5, 0.2)
    float_curve.mapping.update()
    float_curve.mute = True
    require(
        [
            (link.from_socket.identifier, link.to_socket.identifier)
            for link in float_curve.internal_links
        ]
        == [("Value", "Value")],
        "Muted Float Curve must bypass Value, never Factor",
    )
    float_curve.mute = False
    threshold.operation = "LESS"
    threshold.inputs["Else 0"].default_value = False
    threshold.inputs["Value"].default_value = -2.0
    threshold.inputs["Threshold"].default_value = 0.0
    ranged_threshold.operation = "INSIDE"
    ranged_threshold.inputs["Value"].default_value = 0.5
    ranged_threshold.inputs["Min"].default_value = 0.0
    ranged_threshold.inputs["Max"].default_value = 1.0
    within_range.operation = "OUTSIDE"
    within_range.inputs["Value"].default_value = 2.0
    within_range.inputs["Min"].default_value = -1.0
    within_range.inputs["Max"].default_value = 1.0
    value_output(target_position).default_value = (1.0, 2.0, 3.0)
    get_prop_float.inputs["Property"].default_value = "speed"
    get_prop_bool.inputs["Property"].default_value = "enabled"
    get_prop_int.inputs["Property"].default_value = "count"
    get_prop_string.inputs["Property"].default_value = "state"
    set_prop_float.inputs["Property"].default_value = "speed"
    set_prop_bool.inputs["Property"].default_value = "enabled"
    set_prop_int.inputs["Property"].default_value = "count"
    set_prop_string.inputs["Property"].default_value = "state"
    set_prop_string.inputs["Value"].default_value = "running"
    vector_math.operation = "MAXIMUM"
    has_property.inputs["Name"].default_value = "enabled"
    toggle_property.inputs["Property"].default_value = "enabled"
    modify_property.inputs["Property"].default_value = "speed"
    keyboard_key.inputs["Key"].default_value = "A"
    mouse_button.inputs["Button"].default_value = "LEFTMOUSE"
    gamepad_button.gamepad_button = "LT_L2"
    gamepad_button.input_type = "UP"
    # The UI-input flag shares custom1 with the input type: each keeps the other.
    gamepad_button.use_ui_input = True
    keyboard_key.input_type = "DOWN"
    keyboard_key.use_ui_input = True
    keyboard_key.input_type = "UP"
    input_action.inputs["Action"].default_value = "Jump"
    input_action.input_type = "DOWN"
    input_action.use_ui_input = True
    get_input_binding.input_device = "GAMEPAD"
    get_input_binding.input_direction = "LEFT"
    rebind_input_action.rebind_device = "GAMEPAD"
    rebind_input_action.input_direction = "RIGHT"
    input_map_path = socket_by_identifier(input_map.inputs, "Path", "LogicNativeInputMap")
    input_map.input_map_operation = "RESET"
    require(input_map_path.is_unavailable, "Input Map Reset still shows its Path")
    input_map.input_map_operation = "LOAD"
    require(not input_map_path.is_unavailable, "Input Map Load hides its Path")
    binding_object = get_or_create_cube(LOGIC_NODES_BINDING_OBJECT)
    add_object.inputs["Object to Add"].default_value = binding_object
    add_object.inputs["Copy Transform"].default_value = binding_object
    set_parent.inputs["Child Object"].default_value = binding_object
    set_parent.inputs["Parent Object"].default_value = binding_object
    remove_parent.inputs["Child Object"].default_value = binding_object
    remove_object.inputs["Object"].default_value = binding_object
    set_camera_fov.inputs["FOV"].default_value = 60.0
    set_camera_ortho_scale.inputs["Scale"].default_value = 4.0
    world_to_screen.inputs["Point"].default_value = (0.0, 0.0, 0.0)
    screen_to_world.inputs["Screen X"].default_value = 0.5
    screen_to_world.inputs["Screen Y"].default_value = 0.5
    screen_to_world.inputs["Depth"].default_value = 5.0
    set_vsync.vsync_mode = "ADAPTIVE"
    show_framerate.inputs["Show"].default_value = False
    show_profile.inputs["Show"].default_value = False
    get_character_info.use_local_space = False
    set_character_gravity.inputs["Gravity"].default_value = (0.0, 0.0, -12.0)
    set_character_jump_speed.inputs["Force"].default_value = 8.5
    set_character_max_jumps.inputs["Max Jumps"].default_value = 3
    set_character_walk_direction.use_local_space = False
    set_character_walk_direction.inputs["Vector"].default_value = (1.0, 0.0, 0.0)
    set_character_velocity.use_local_space = False
    set_character_velocity.inputs["Velocity"].default_value = (0.0, 2.0, 0.0)
    set_character_velocity.inputs["Time"].default_value = 0.25
    vehicle_control.inputs["Throttle"].default_value = 0.8
    vehicle_control.inputs["Brake"].default_value = 0.35
    vehicle_control.inputs["Handbrake"].default_value = 0.15
    vehicle_control.inputs["Steering"].default_value = -0.25
    vehicle_accelerate.vehicle_axis = "ALL"
    vehicle_accelerate.inputs["Wheels"].default_value = 4
    vehicle_accelerate.inputs["Power"].default_value = 0.75
    vehicle_brake.vehicle_axis = "FRONT"
    vehicle_brake.inputs["Power"].default_value = 0.5
    vehicle_steer.vehicle_axis = "FRONT"
    vehicle_steer.inputs["Steer"].default_value = 0.25
    vehicle_set_attributes.vehicle_axis = "ALL"
    vehicle_set_attributes.inputs["Suspension"].default_value = True
    vehicle_set_attributes.inputs["Suspension Value"].default_value = 0.35
    vehicle_set_attributes.inputs["Stiffness"].default_value = True
    vehicle_set_attributes.inputs["Stiffness Value"].default_value = 15000.0
    vehicle_set_attributes.inputs["Damping"].default_value = True
    vehicle_set_attributes.inputs["Damping Value"].default_value = 2500.0
    vehicle_set_attributes.inputs["Friction"].default_value = True
    vehicle_set_attributes.inputs["Friction Value"].default_value = 10.5

    linked_tree.links.new(on_fixed_update.outputs["Out"], branch.inputs["Flow"])
    linked_tree.links.new(value_output(condition), gate.inputs["Conditions"])
    linked_tree.links.new(value_output(get_prop_bool), gate.inputs["Conditions"])
    linked_tree.links.new(gate.outputs["Result"], branch.inputs["Condition"])
    linked_tree.links.new(value_output(value_a), math.inputs["A"])
    linked_tree.links.new(value_output(value_b), math.inputs["B"])
    linked_tree.links.new(branch.outputs["True"], flow_input(set_position))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_position))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_world_orientation))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_orientation))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_world_scale))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_scale))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_visibility))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_object_color))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(make_light_unique))
    linked_tree.links.new(make_light_unique.outputs["Out"], flow_input(set_light_color))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(set_light_power))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(set_light_shadow))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_velocity))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_velocity))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_angular_velocity))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_angular_velocity))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_world_transform))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_local_transform))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_movement))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_rotation))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_force))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_torque))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_impulse))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_prop_int))
    linked_tree.links.new(get_position.outputs["Vector"], vector_math.inputs["A"])
    linked_tree.links.new(value_output(target_position), vector_math.inputs["B"])
    linked_tree.links.new(value_output(target_position), set_local_position.inputs["Value"])
    linked_tree.links.new(euler.outputs["Rotation"], set_world_orientation.inputs["Rotation"])
    linked_tree.links.new(
        get_local_orientation.outputs["Orientation"], set_local_orientation.inputs["Rotation"]
    )
    linked_tree.links.new(get_world_scale.outputs["Vector"], set_world_scale.inputs["Value"])
    linked_tree.links.new(get_local_scale.outputs["Vector"], set_local_scale.inputs["Value"])
    linked_tree.links.new(get_visibility.outputs["Visible"], set_visibility.inputs["Visible"])
    linked_tree.links.new(color_rgba.outputs["Color"], set_object_color.inputs["Color"])
    linked_tree.links.new(get_light_color.outputs["Color"], set_light_color.inputs["Color"])
    linked_tree.links.new(get_light_power.outputs["Power"], set_light_power.inputs["Power"])
    linked_tree.links.new(combine_xy.outputs["Vector"], separate_xy.inputs["Vector"])
    linked_tree.links.new(combine_xyz.outputs["Vector"], separate_xyz.inputs["Vector"])
    linked_tree.links.new(vector_math.outputs["Result"], set_position.inputs["Value"])
    linked_tree.links.new(get_velocity.outputs["Vector"], set_velocity.inputs["Value"])
    linked_tree.links.new(get_local_velocity.outputs["Vector"], set_local_velocity.inputs["Value"])
    linked_tree.links.new(get_angular_velocity.outputs["Vector"], set_angular_velocity.inputs["Value"])
    linked_tree.links.new(
        get_local_angular_velocity.outputs["Vector"],
        set_local_angular_velocity.inputs["Value"],
    )
    linked_tree.links.new(value_output(target_position), vector_input(apply_movement, "Vector", "Movement"))
    linked_tree.links.new(
        get_world_orientation.outputs["Orientation"], apply_rotation.inputs["Rotation"]
    )
    linked_tree.links.new(value_output(target_position), apply_force.inputs["Force"])
    linked_tree.links.new(value_output(target_position), apply_torque.inputs["Torque"])
    linked_tree.links.new(value_output(target_position), apply_impulse.inputs["Attach"])
    linked_tree.links.new(value_output(target_position), apply_impulse.inputs["Impulse"])
    linked_tree.links.new(value_output(value_int), set_prop_int.inputs["Value"])
    linked_tree.links.new(value_output(value_string), set_prop_string.inputs["Value"])
    linked_tree.links.new(on_update.outputs["Out"], flow_input(set_fullscreen))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(set_resolution))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(set_vsync))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(show_framerate))
    linked_tree.links.new(on_update.outputs["Out"], flow_input(show_profile))
    linked_tree.links.new(get_fullscreen.outputs["Fullscreen"], set_fullscreen.inputs["Fullscreen"])
    linked_tree.links.new(get_resolution.outputs["Width"], set_resolution.inputs["X"])
    linked_tree.links.new(get_resolution.outputs["Height"], set_resolution.inputs["Y"])
    linked_tree.links.new(on_update.outputs["Out"], sequence.inputs["Flow"])
    linked_tree.links.new(sequence.outputs["Then 0"], flow_gate.inputs["Open"])
    linked_tree.links.new(on_update.outputs["Out"], any_flow.inputs["In"])
    linked_tree.links.new(sequence.outputs["Then 1"], any_flow.inputs["In"])
    linked_tree.links.new(any_flow.outputs["Out"], flow_gate.inputs["Enter"])

    build_binding_probe(linked_tree)
    EXPECTED_LINKED_NODE_COUNT = len(linked_tree.nodes)
    EXPECTED_LINKED_LINK_COUNT = len(linked_tree.links)


def find_node(tree, bl_idname):
    for node in tree.nodes:
        if node.bl_idname == bl_idname:
            return node
    return None


def find_nodes(tree, bl_idname):
    return [node for node in tree.nodes if node.bl_idname == bl_idname]


def validate_probe_trees():
    empty_tree = bpy.data.node_groups.get("LN Empty Tree")
    single_node_tree = bpy.data.node_groups.get("LN Single Node Tree")
    linked_tree = bpy.data.node_groups.get("LN Linked Tree")

    require(empty_tree is not None, "Empty LogicNodeTree did not survive save/load")
    require(single_node_tree is not None, "Single-node LogicNodeTree did not survive save/load")
    require(linked_tree is not None, "Linked LogicNodeTree did not survive save/load")
    require(empty_tree.bl_idname == "LogicNodeTree", "Empty tree lost LogicNodeTree type")
    require(
        single_node_tree.bl_idname == "LogicNodeTree",
        "Single-node tree lost LogicNodeTree type",
    )
    require(linked_tree.bl_idname == "LogicNodeTree", "Linked tree lost LogicNodeTree type")
    require(len(empty_tree.nodes) == 0, "Empty tree should not gain nodes")
    require(len(single_node_tree.nodes) == 1, "Single-node tree should keep exactly one node")
    require(single_node_tree.nodes[0].bl_idname == "LogicNativeOnInit", "Wrong single node type")
    require(EXPECTED_LINKED_NODE_COUNT is not None, "Linked-tree node baseline was not recorded")
    require(EXPECTED_LINKED_LINK_COUNT is not None, "Linked-tree link baseline was not recorded")
    require(
        len(linked_tree.nodes) == EXPECTED_LINKED_NODE_COUNT,
        "Linked tree did not preserve every probe node across save/load",
    )
    require(
        len(linked_tree.links) == EXPECTED_LINKED_LINK_COUNT,
        "Linked tree did not preserve every probe link across save/load",
    )

    validate_probe_tree_socket_idnames(single_node_tree)
    validate_probe_tree_socket_idnames(linked_tree)

    math = find_node(linked_tree, "LogicNativeMath")
    require(math is not None, "Math node did not survive save/load")
    require(math.operation == "MAXIMUM", "Math node lost operation")
    vector_math = find_node(linked_tree, "LogicNativeVectorMath")
    require(vector_math is not None, "Vector Math node did not survive save/load")
    require(vector_math.operation == "MAXIMUM", "Vector Math node lost operation")
    float_curve = find_node(linked_tree, "LogicNativeFloatCurve")
    require(float_curve is not None, "Float Curve node did not survive save/load")
    require(
        [socket.identifier for socket in float_curve.inputs] == ["Factor", "Value"],
        "Float Curve input socket contract changed",
    )
    require(
        [socket.identifier for socket in float_curve.outputs] == ["Value"],
        "Float Curve output socket contract changed",
    )
    require(
        abs(float_curve.inputs["Factor"].default_value - 0.4) < 1.0e-6,
        "Float Curve lost its Factor value",
    )
    require(
        abs(float_curve.inputs["Value"].default_value - 0.25) < 1.0e-6,
        "Float Curve lost its input Value",
    )
    require(
        float_curve.mapping.extend == "HORIZONTAL",
        "Float Curve lost its horizontal extension mode",
    )
    require(float_curve.mapping.use_clip is False, "Float Curve lost its clipping mode")
    curve_points = [tuple(point.location) for point in float_curve.mapping.curves[0].points]
    require(len(curve_points) == 3, "Float Curve lost an authored control point")
    require(
        any(abs(x - 0.5) < 1.0e-6 and abs(y - 0.2) < 1.0e-6 for x, y in curve_points),
        "Float Curve changed its authored control point",
    )
    gate = find_node(linked_tree, "LogicNativeGate")
    require(gate is not None, "Gate node did not survive save/load")
    require(gate.operation == "AND", "Gate node lost operation")
    gate_conditions = gate.inputs["Conditions"]
    require(gate_conditions.is_multi_input, "Boolean Combine lost its multi-input socket")
    require(
        sum(link.to_socket == gate_conditions for link in linked_tree.links) == 2,
        "Boolean Combine did not preserve both condition links",
    )
    flow_gate = find_node(linked_tree, "LogicNativeFlowGate")
    require(flow_gate is not None, "Flow Gate node did not survive save/load")
    require(flow_gate.inputs["Start Open"].default_value is False, "Flow Gate lost initial state")
    any_flow = find_node(linked_tree, "LogicNativeAnyFlow")
    require(any_flow is not None, "Any Flow node did not survive save/load")
    any_flow_input = any_flow.inputs["In"]
    require(any_flow_input.is_multi_input, "Any Flow lost its multi-input socket")
    require(
        sum(link.to_socket == any_flow_input for link in linked_tree.links) == 2,
        "Any Flow did not preserve both execution links",
    )
    sequence = find_node(linked_tree, "LogicNativeSequence")
    require(sequence is not None, "Sequence node did not survive save/load")
    require(
        [socket.identifier for socket in sequence.outputs] == ["Then 0", "Then 1"],
        "Sequence did not preserve its ordered output count",
    )
    expected_flow_links = {
        ("LogicNativeOnFixedUpdate", "Out", "LogicNativeBranch", "Flow"),
        ("LogicNativeOnUpdate", "Out", "LogicNativeSequence", "Flow"),
        ("LogicNativeSequence", "Then 0", "LogicNativeFlowGate", "Open"),
        ("LogicNativeOnUpdate", "Out", "LogicNativeAnyFlow", "In"),
        ("LogicNativeSequence", "Then 1", "LogicNativeAnyFlow", "In"),
        ("LogicNativeAnyFlow", "Out", "LogicNativeFlowGate", "Enter"),
    }
    actual_flow_links = {
        (
            link.from_node.bl_idname,
            link.from_socket.identifier,
            link.to_node.bl_idname,
            link.to_socket.identifier,
        )
        for link in linked_tree.links
    }
    require(
        expected_flow_links <= actual_flow_links,
        "Execution-flow links did not survive save/load exactly",
    )
    for node_idname in ("LogicNativeKeyboardKey", "LogicNativeMouseButton"):
        input_node = find_node(linked_tree, node_idname)
        require(input_node is not None, f"{node_idname} did not survive save/load")
        triggered = next(socket for socket in input_node.outputs if socket.identifier == "Out")
        require(triggered.name == "Triggered", f"{node_idname} lost event label")
        require("Active" in input_node.outputs, f"{node_idname} lost Active state output")
    gamepad_button = find_node(linked_tree, "LogicNativeGamepadButton")
    require(gamepad_button is not None, "Gamepad Button node did not survive save/load")
    gamepad_triggered = next(
        socket for socket in gamepad_button.outputs if socket.identifier == "Out"
    )
    require(gamepad_triggered.name == "Triggered", "Gamepad event label changed")
    require("Active" in gamepad_button.outputs, "Gamepad Button lost Active state output")
    require(gamepad_button.gamepad_button == "LT_L2", "Gamepad Button lost selected trigger")
    require(gamepad_button.input_type == "UP", "Gamepad Button lost Released mode")
    require(gamepad_button.use_ui_input, "Gamepad Button lost Also When UI Uses It")
    keyboard_key = find_node(linked_tree, "LogicNativeKeyboardKey")
    require(
        keyboard_key.input_type == "UP" and keyboard_key.use_ui_input,
        "Keyboard Key input type and Also When UI Uses It overwrote each other",
    )
    input_action = find_node(linked_tree, "LogicNativeInputAction")
    require(input_action is not None, "Input Action node did not survive save/load")
    require(input_action.inputs["Action"].default_value == "Jump", "Input Action lost its action")
    require(
        input_action.input_type == "DOWN" and input_action.use_ui_input,
        "Input Action lost its settings",
    )
    get_input_binding = find_node(linked_tree, "LogicNativeGetInputBinding")
    require(
        get_input_binding.input_device == "GAMEPAD"
        and get_input_binding.input_direction == "LEFT",
        "Get Input Binding lost its settings",
    )
    rebind_input_action = find_node(linked_tree, "LogicNativeRebindInputAction")
    require(
        rebind_input_action.rebind_device == "GAMEPAD"
        and rebind_input_action.input_direction == "RIGHT",
        "Rebind Input Action lost its settings",
    )
    input_map = find_node(linked_tree, "LogicNativeInputMap")
    require(input_map.input_map_operation == "LOAD", "Input Map lost its operation")
    euler = find_node(linked_tree, "LogicNativeEuler")
    require(euler is not None, "Combine Rotation node did not survive save/load")
    require(euler.bl_label == "Combine Rotation", "Combine Rotation label did not survive")
    require(
        euler.outputs["Rotation"].name == "Rotation",
        "Combine Rotation output label did not survive",
    )
    require(
        all(socket.bl_idname == "NodeSocketFloatAngle" for socket in euler.inputs),
        "Combine Rotation angle socket types did not survive",
    )
    require(
        abs(euler.inputs["Z"].default_value - 0.3) <= 0.0001,
        "Combine Rotation node lost its radian input value",
    )
    color_rgba = find_node(linked_tree, "LogicNativeColorRGBA")
    require(color_rgba is not None, "Color RGBA node did not survive save/load")
    require(abs(color_rgba.inputs["A"].default_value - 1.0) <= 0.0001, "Color RGBA lost alpha")
    string_operation = find_node(linked_tree, "LogicNativeStringOperation")
    require(string_operation is not None, "String Operation node did not survive save/load")
    require(string_operation.operation == "REPLACE", "String Operation node lost operation")
    formatted_string = find_node(linked_tree, "LogicNativeFormattedString")
    require(formatted_string is not None, "Formatted String node did not survive save/load")
    require(
        formatted_string.inputs["Format String"].default_value == "A is {} and B is {}",
        "Formatted String node lost format input",
    )
    require(
        find_node(linked_tree, "LogicNativeGetGamePropertyFloat") is not None,
        "Get Game Property Float node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetGamePropertyBool") is not None,
        "Get Game Property Bool node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetGamePropertyInt") is not None,
        "Get Game Property Int node did not survive save/load",
    )
    value_changed = find_node(linked_tree, "LogicNativeValueChanged")
    require(value_changed is not None, "On Value Changed node did not survive save/load")
    require(
        value_changed.ignore_initial_none is True,
        "On Value Changed lost its Ignore Initial None option",
    )
    value_changed_to = find_node(linked_tree, "LogicNativeValueChangedTo")
    require(value_changed_to is not None, "On Value Changed To node did not survive save/load")
    require(
        value_changed_to.inputs["Target"].name == "Target",
        "On Value Changed To did not repair or preserve its Target input label",
    )
    require(
        value_changed_to.outputs["Result"].name == "If Changed To",
        "On Value Changed To did not repair or preserve its result output label",
    )
    require(
        find_node(linked_tree, "LogicNativeGetGamePropertyString") is not None,
        "Get Game Property String node did not survive save/load",
    )
    get_attribute_modes = {
        node.attribute_type
        for node in find_nodes(linked_tree, "LogicNativeGetObjectAttribute")
    }
    expected_get_attribute_modes = {
        "WORLD_POSITION",
        "LOCAL_POSITION",
        "VISIBLE",
        "NAME",
        "LOCAL_SCALE",
        "WORLD_SCALE",
        "COLOR",
        "LOCAL_ORIENTATION",
        "WORLD_ORIENTATION",
        "WORLD_LINEAR_VELOCITY",
        "LOCAL_LINEAR_VELOCITY",
        "WORLD_ANGULAR_VELOCITY",
        "LOCAL_ANGULAR_VELOCITY",
        "WORLD_TRANSFORM",
        "LOCAL_TRANSFORM",
    }
    require(
        get_attribute_modes == expected_get_attribute_modes,
        f"Get Object Attribute modes did not survive save/load: {get_attribute_modes}",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLightColor") is not None,
        "Get Light Color node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLightPower") is not None,
        "Get Light Power node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetGamePropertyFloat") is not None,
        "Set Game Property Float node did not survive save/load",
    )
    set_attribute_modes = {
        node.attribute_type
        for node in find_nodes(linked_tree, "LogicNativeSetObjectAttribute")
    }
    expected_set_attribute_modes = {
        "WORLD_POSITION",
        "WORLD_ORIENTATION",
        "WORLD_LINEAR_VELOCITY",
        "WORLD_ANGULAR_VELOCITY",
        "WORLD_TRANSFORM",
        "WORLD_SCALE",
        "LOCAL_POSITION",
        "LOCAL_ORIENTATION",
        "LOCAL_LINEAR_VELOCITY",
        "LOCAL_ANGULAR_VELOCITY",
        "LOCAL_TRANSFORM",
        "COLOR",
        "VISIBLE",
        "LOCAL_SCALE",
    }
    require(
        set_attribute_modes == expected_set_attribute_modes,
        f"Set Object Attribute modes did not survive save/load: {set_attribute_modes}",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLightColor") is not None,
        "Set Light Color node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLightPower") is not None,
        "Set Light Power node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLightShadow") is not None,
        "Set Light Shadow node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeMakeLightUnique") is not None,
        "Make Light Unique node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeApplyMovement") is not None,
        "Apply Movement node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeApplyRotation") is not None,
        "Apply Rotation node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeApplyForce") is not None,
        "Apply Force node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeApplyTorque") is not None,
        "Apply Torque node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetGamePropertyBool") is not None,
        "Set Game Property Bool node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetGamePropertyInt") is not None,
        "Set Game Property Int node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetGamePropertyString") is not None,
        "Set Game Property String node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetFullscreen") is not None,
        "Get Fullscreen node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetResolution") is not None,
        "Get Resolution node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetVSync") is not None,
        "Get VSync node did not survive save/load",
    )
    set_vsync = find_node(linked_tree, "LogicNativeSetVSync")
    require(set_vsync is not None, "Set VSync node did not survive save/load")
    require(set_vsync.vsync_mode == "ADAPTIVE", "Set VSync node lost mode")
    get_character_info = find_node(linked_tree, "LogicNativeGetCharacterInfo")
    require(get_character_info is not None, "Get Character Info node did not survive save/load")
    require(get_character_info.use_local_space is False, "Get Character Info node lost local mode")
    require(
        find_node(linked_tree, "LogicNativeCharacterJump") is not None,
        "Character Jump node did not survive save/load",
    )
    set_character_gravity = find_node(linked_tree, "LogicNativeSetCharacterGravity")
    require(set_character_gravity is not None, "Set Character Gravity node did not survive save/load")
    require(
        abs(set_character_gravity.inputs["Gravity"].default_value[2] + 12.0) <= 0.0001,
        "Set Character Gravity node lost gravity input",
    )
    set_character_jump_speed = find_node(linked_tree, "LogicNativeSetCharacterJumpSpeed")
    require(
        set_character_jump_speed is not None,
        "Set Character Jump Speed node did not survive save/load",
    )
    require(
        abs(set_character_jump_speed.inputs["Force"].default_value - 8.5) <= 0.0001,
        "Set Character Jump Speed node lost force input",
    )
    set_character_max_jumps = find_node(linked_tree, "LogicNativeSetCharacterMaxJumps")
    require(
        set_character_max_jumps is not None,
        "Set Character Max Jumps node did not survive save/load",
    )
    require(
        set_character_max_jumps.inputs["Max Jumps"].default_value == 3,
        "Set Character Max Jumps node lost jump count input",
    )
    set_character_walk_direction = find_node(linked_tree, "LogicNativeSetCharacterWalkDirection")
    require(
        set_character_walk_direction is not None,
        "Set Character Walk Direction node did not survive save/load",
    )
    require(
        set_character_walk_direction.use_local_space is False,
        "Set Character Walk Direction node lost local mode",
    )
    set_character_velocity = find_node(linked_tree, "LogicNativeSetCharacterVelocity")
    require(
        set_character_velocity is not None,
        "Set Character Velocity node did not survive save/load",
    )
    require(
        set_character_velocity.use_local_space is False,
        "Set Character Velocity node lost local mode",
    )
    require(
        abs(set_character_velocity.inputs["Time"].default_value - 0.25) <= 0.0001,
        "Set Character Velocity node lost time input",
    )
    vehicle_control = find_node(linked_tree, "LogicNativeVehicleControl")
    require(vehicle_control is not None, "Vehicle Control node did not survive save/load")
    require(
        abs(vehicle_control.inputs["Throttle"].default_value - 0.8) <= 0.0001,
        "Vehicle Control node lost throttle input",
    )
    require(
        abs(vehicle_control.inputs["Brake"].default_value - 0.35) <= 0.0001,
        "Vehicle Control node lost brake input",
    )
    require(
        abs(vehicle_control.inputs["Handbrake"].default_value - 0.15) <= 0.0001,
        "Vehicle Control node lost handbrake input",
    )
    require(
        abs(vehicle_control.inputs["Steering"].default_value + 0.25) <= 0.0001,
        "Vehicle Control node lost steering input",
    )
    vehicle_accelerate = find_node(linked_tree, "LogicNativeVehicleAccelerate")
    require(vehicle_accelerate is not None, "Vehicle Accelerate node did not survive save/load")
    require(vehicle_accelerate.vehicle_axis == "ALL", "Vehicle Accelerate node lost axis")
    vehicle_brake = find_node(linked_tree, "LogicNativeVehicleBrake")
    require(vehicle_brake is not None, "Vehicle Brake node did not survive save/load")
    require(vehicle_brake.vehicle_axis == "FRONT", "Vehicle Brake node lost axis")
    vehicle_steer = find_node(linked_tree, "LogicNativeVehicleSteer")
    require(vehicle_steer is not None, "Vehicle Steer node did not survive save/load")
    require(vehicle_steer.vehicle_axis == "FRONT", "Vehicle Steer node lost axis")
    vehicle_set_attributes = find_node(linked_tree, "LogicNativeVehicleSetAttributes")
    require(
        vehicle_set_attributes is not None,
        "Vehicle Set Attributes node did not survive save/load",
    )
    require(
        vehicle_set_attributes.vehicle_axis == "ALL",
        "Vehicle Set Attributes node lost axis",
    )
    require(
        vehicle_set_attributes.inputs["Suspension"].default_value is True,
        "Vehicle Set Attributes node lost suspension toggle",
    )
    require(
        find_node(linked_tree, "LogicNativeShowFramerate") is not None,
        "Show Framerate node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeShowProfile") is not None,
        "Show Profile node did not survive save/load",
    )
    raycast_from_mesh = find_node(linked_tree, "LogicNativeRaycastFromMesh")
    require(raycast_from_mesh is not None, "Raycast from Mesh node did not survive save/load")
    require(raycast_from_mesh.mesh_domain == "FACES", "Raycast from Mesh lost its mesh domain")
    require(
        raycast_from_mesh.ray_direction == "INVERTED_NORMALS",
        "Raycast from Mesh lost its direction",
    )
    require(
        abs(raycast_from_mesh.inputs["Start Offset"].default_value - 0.125) <= 0.0001,
        "Raycast from Mesh lost Start Offset",
    )
    require(
        abs(raycast_from_mesh.inputs["Max Distance"].default_value - 42.0) <= 0.0001,
        "Raycast from Mesh lost Max Distance",
    )
    require(
        raycast_from_mesh.inputs["Max Rays"].default_value == 17,
        "Raycast from Mesh lost Max Rays",
    )

    binding_object = bpy.data.objects.get(LOGIC_NODES_BINDING_OBJECT)
    require(binding_object is not None, "Binding probe object did not survive save/load")
    require(
        len(binding_object.game.logic_node_bindings) == 1,
        "Logic node binding collection was not persisted",
    )
    binding = binding_object.game.logic_node_bindings[0]
    require(
        binding.tree == linked_tree,
        "Logic node binding lost its node-tree identity across save/load",
    )
    require(binding.tree_name == "LN Linked Tree", "Tree binding value was not persisted")
    require(binding.enabled is True, "Enabled binding value was not persisted")

    clear_object = bpy.data.objects.get(LOGIC_NODES_CLEAR_OBJECT)
    require(clear_object is not None, "Clear probe object did not survive save/load")
    require(
        len(clear_object.game.logic_node_bindings) == 0,
        "Cleared logic node bindings survived",
    )

    result = bpy.ops.node.validate_logic_tree(tree_name=linked_tree.name)
    require(result == {"FINISHED"}, f"New-format linked LogicNodeTree failed validation: {result}")


def validate_probe_tree_socket_idnames(tree):
    for node in tree.nodes:
        if node.bl_idname == "LogicNativeGetObjectAttribute":
            output_contracts = {
                "WORLD_POSITION": {"Vector": "NodeSocketLogicVector"},
                "LOCAL_POSITION": {"Vector": "NodeSocketLogicVector"},
                "VISIBLE": {"Visible": "NodeSocketLogicBool"},
                "NAME": {"Name": "NodeSocketLogicString"},
                "LOCAL_SCALE": {"Vector": "NodeSocketLogicVector"},
                "WORLD_SCALE": {"Vector": "NodeSocketLogicVector"},
                "COLOR": {"Color": "NodeSocketLogicColor"},
                "LOCAL_ORIENTATION": {"Orientation": "NodeSocketLogicRotation"},
                "WORLD_ORIENTATION": {"Orientation": "NodeSocketLogicRotation"},
                "WORLD_LINEAR_VELOCITY": {"Vector": "NodeSocketLogicVector"},
                "LOCAL_LINEAR_VELOCITY": {"Vector": "NodeSocketLogicVector"},
                "WORLD_ANGULAR_VELOCITY": {"Vector": "NodeSocketLogicVector"},
                "LOCAL_ANGULAR_VELOCITY": {"Vector": "NodeSocketLogicVector"},
                "WORLD_TRANSFORM": {
                    "Position": "NodeSocketLogicVector",
                    "Orientation": "NodeSocketLogicRotation",
                    "Scale": "NodeSocketLogicVector",
                },
                "LOCAL_TRANSFORM": {
                    "Position": "NodeSocketLogicVector",
                    "Orientation": "NodeSocketLogicRotation",
                    "Scale": "NodeSocketLogicVector",
                },
            }
            expected = {
                "inputs": {"Object": "NodeSocketLogicObject"},
                "outputs": output_contracts[node.attribute_type],
            }
        elif node.bl_idname == "LogicNativeSetObjectAttribute":
            common_inputs = {
                "Flow": "NodeSocketLogicExecution",
                "Object": "NodeSocketLogicObject",
            }
            vector_inputs = {
                **common_inputs,
                "XYZ": "NodeSocketLogicVector",
                "Value": "NodeSocketLogicVector",
            }
            rotation_inputs = {
                **common_inputs,
                "XYZ": "NodeSocketLogicVector",
                "Rotation": "NodeSocketLogicRotation",
            }
            transform_inputs = {
                **common_inputs,
                "Position": "NodeSocketLogicVector",
                "Rotation": "NodeSocketLogicRotation",
                "Scale": "NodeSocketLogicVector",
            }
            input_contracts = {
                "WORLD_POSITION": vector_inputs,
                "WORLD_ORIENTATION": rotation_inputs,
                "WORLD_LINEAR_VELOCITY": vector_inputs,
                "WORLD_ANGULAR_VELOCITY": vector_inputs,
                "WORLD_TRANSFORM": transform_inputs,
                "WORLD_SCALE": vector_inputs,
                "LOCAL_POSITION": vector_inputs,
                "LOCAL_ORIENTATION": rotation_inputs,
                "LOCAL_LINEAR_VELOCITY": vector_inputs,
                "LOCAL_ANGULAR_VELOCITY": vector_inputs,
                "LOCAL_TRANSFORM": transform_inputs,
                "COLOR": {
                    **common_inputs,
                    "XYZ": "NodeSocketLogicVector",
                    "Color": "NodeSocketLogicColor",
                },
                "VISIBLE": {
                    **common_inputs,
                    "Visible": "NodeSocketLogicBool",
                    "Include Children": "NodeSocketLogicBool",
                },
                "LOCAL_SCALE": vector_inputs,
            }
            expected = {
                "inputs": input_contracts[node.attribute_type],
                "outputs": {"Done": "NodeSocketLogicExecution"},
            }
        else:
            expected = EXPECTED_SOCKET_IDNAMES.get(node.bl_idname)
        if expected is None:
            continue
        for collection_name in ("inputs", "outputs"):
            expected_sockets = expected.get(collection_name, {})
            sockets = getattr(node, collection_name)
            if node.bl_idname in {
                "LogicNativeGetObjectAttribute",
                "LogicNativeSetObjectAttribute",
            }:
                actual_socket_names = {
                    socket.identifier for socket in sockets if not socket.is_unavailable
                }
                require(
                    actual_socket_names == set(expected_sockets),
                    f"{node.bl_idname} {node.attribute_type} has wrong active "
                    f"{collection_name}: {sorted(actual_socket_names)}",
                )
            for socket_name, expected_idname in expected_sockets.items():
                if node.bl_idname in {
                    "LogicNativeGetObjectAttribute",
                    "LogicNativeSetObjectAttribute",
                    "LogicNativeSetParent",
                }:
                    socket = next(
                        (socket for socket in sockets if socket.identifier == socket_name),
                        None,
                    )
                else:
                    socket = sockets.get(socket_name)
                require(
                    socket is not None,
                    f"{node.bl_idname} is missing {collection_name[:-1]} socket {socket_name}",
                )
                require(
                    socket.bl_idname == expected_idname,
                    f"{node.bl_idname}.{socket_name} socket idname mismatch: {socket.bl_idname}",
                )

                require_color_close(
                    socket.draw_color_simple(),
                    LOGIC_SOCKET_COLORS[expected_idname],
                    f"{node.bl_idname}.{socket_name}",
                )

        for prop_name, expected_items in expected.get("rna_enums", {}).items():
            require(
                prop_name in node.bl_rna.properties,
                f"{node.bl_idname} is missing RNA enum {prop_name}",
            )
            actual_items = {
                item.identifier for item in node.bl_rna.properties[prop_name].enum_items
            }
            require(
                actual_items == set(expected_items),
                f"{node.bl_idname}.{prop_name} enum mismatch: {sorted(actual_items)}",
            )


OBJECT_UI_NAVIGATION_NODE_IDNAMES = (
    "LogicNativeSetOverlayCollection",
    "LogicNativeRemoveOverlayCollection",
    "LogicNativeOpenObjectMenu",
    "LogicNativeCloseObjectMenu",
    "LogicNativeSetUIFocus",
    "LogicNativeGetUIFocus",
    "LogicNativeScreenAnchor",
    "LogicNativeObjectScrollView",
    "LogicNativeObjectGridView",
)

FORBIDDEN_OBJECT_UI_LAYER_SOCKETS = {
    "UI Layer",
    "Layer",
    "Order",
    "UI Order",
    "Visual Layer",
}


def validate_object_ui_navigation_node_sockets():
    tree = new_logic_tree("LN Object UI Navigation Socket Audit")
    collection = bpy.data.collections.new("LN Overlay Menu Probe")
    camera_data = bpy.data.cameras.new("LN Overlay Camera")
    camera = bpy.data.objects.new("LN Overlay Camera Probe", camera_data)
    nodes = {}
    try:
        for idname in OBJECT_UI_NAVIGATION_NODE_IDNAMES:
            expected = EXPECTED_SOCKET_IDNAMES[idname]
            node = tree.nodes.new(idname)
            nodes[idname] = node
            for collection_name in ("inputs", "outputs"):
                expected_sockets = expected.get(collection_name, {})
                sockets = getattr(node, collection_name)
                actual_names = [socket.identifier for socket in sockets]
                require(
                    actual_names == list(expected_sockets),
                    f"{idname} {collection_name} changed: {actual_names}",
                )
                for socket in sockets:
                    require(
                        socket.identifier not in FORBIDDEN_OBJECT_UI_LAYER_SOCKETS,
                        f"{idname} shipped a visual-layer socket {socket.identifier}",
                    )
                    expected_idname = expected_sockets[socket.identifier]
                    require(
                        socket.bl_idname == expected_idname,
                        f"{idname}.{socket.identifier} socket idname mismatch: {socket.bl_idname}",
                    )
                    require_color_close(
                        socket.draw_color_simple(),
                        LOGIC_SOCKET_COLORS[expected_idname],
                        f"{idname}.{socket.identifier}",
                    )
            for prop_name, expected_items in expected.get("rna_enums", {}).items():
                require(
                    prop_name in node.bl_rna.properties,
                    f"{idname} is missing RNA enum {prop_name}",
                )
                actual_items = {
                    item.identifier for item in node.bl_rna.properties[prop_name].enum_items
                }
                require(
                    actual_items == set(expected_items),
                    f"{idname}.{prop_name} enum mismatch: {sorted(actual_items)}",
                )

        set_overlay = nodes["LogicNativeSetOverlayCollection"]
        remove_overlay = nodes["LogicNativeRemoveOverlayCollection"]
        open_menu = nodes["LogicNativeOpenObjectMenu"]
        close_menu = nodes["LogicNativeCloseObjectMenu"]
        set_focus = nodes["LogicNativeSetUIFocus"]
        screen_anchor = nodes["LogicNativeScreenAnchor"]

        require("Done" not in set_overlay.outputs, "Set Overlay Collection must not expose Done")
        require(
            "Done" not in remove_overlay.outputs,
            "Remove Overlay Collection must not expose Done",
        )
        require(open_menu.menu_routing == "MODAL", "Open Object Menu routing should default to Modal")
        require(open_menu.menu_open_mode == "PUSH", "Open Object Menu should default to Push")
        require(
            open_menu.menu_presentation == "SCREEN",
            "Open Object Menu should default to Screen",
        )
        require(
            open_menu.menu_back_behavior == "CLOSE_MENU",
            "Open Object Menu should default to Close Menu",
        )
        open_menu.menu_open_mode = "REPLACE_TOP"
        open_menu.menu_presentation = "OVERLAY"
        open_menu.menu_back_behavior = "CUSTOM"
        open_menu.menu_routing = "PASS_THROUGH"
        require(open_menu.menu_open_mode == "REPLACE_TOP", "Open Mode did not keep Replace Top")
        require(open_menu.menu_presentation == "OVERLAY", "Presentation did not keep Overlay")
        require(open_menu.menu_back_behavior == "CUSTOM", "Back did not keep Custom")
        require(open_menu.menu_routing == "PASS_THROUGH", "Routing did not keep Pass Through")
        open_menu.menu_routing = "MODAL"
        require(
            open_menu.menu_open_mode == "REPLACE_TOP"
            and open_menu.menu_presentation == "OVERLAY"
            and open_menu.menu_back_behavior == "CUSTOM",
            "Changing Routing mutated packed Open Object Menu settings",
        )
        open_menu.menu_back_behavior = "IGNORE"
        require(
            open_menu.menu_open_mode == "REPLACE_TOP" and open_menu.menu_presentation == "OVERLAY",
            "Changing Back mutated other packed Open Object Menu settings",
        )

        require(close_menu.menu_close_operation == "TOP", "Close Object Menu should default to Top")
        menu_socket = socket_by_identifier(close_menu.inputs, "Menu", close_menu.bl_idname)
        require(
            menu_socket.is_unavailable,
            "Top Menu close should hide the Menu handle socket",
        )
        close_menu.menu_close_operation = "ALL"
        bpy.context.view_layer.update()
        require(
            menu_socket.is_unavailable,
            "All Menus close should hide the Menu handle socket",
        )
        close_menu.menu_close_operation = "SPECIFIC"
        bpy.context.view_layer.update()
        require(
            not menu_socket.is_unavailable,
            "Specific Menu close should expose the Menu handle socket",
        )
        close_top = tree.nodes.new("LogicNativeCloseObjectMenu")
        close_all = tree.nodes.new("LogicNativeCloseObjectMenu")
        close_top.menu_close_operation = "TOP"
        close_all.menu_close_operation = "ALL"
        bpy.context.view_layer.update()

        on_init = tree.nodes.new("LogicNativeOnInit")
        set_overlay.inputs["Collection"].default_value = collection
        remove_overlay.inputs["Collection"].default_value = collection
        open_menu.inputs["Collection"].default_value = collection
        open_menu.inputs["Input Context"].default_value = ""
        set_overlay.inputs["Camera"].default_value = None
        open_menu.inputs["Camera"].default_value = camera

        tree.links.new(on_init.outputs["Out"], flow_input(set_overlay))
        tree.links.new(set_overlay.outputs["Success"], flow_input(remove_overlay))
        tree.links.new(set_overlay.outputs["Failed"], flow_input(close_top))
        tree.links.new(on_init.outputs["Out"], flow_input(open_menu))
        tree.links.new(open_menu.outputs["Opened"], flow_input(close_menu))
        tree.links.new(open_menu.outputs["Menu"], menu_socket)
        tree.links.new(open_menu.outputs["Failed"], flow_input(close_all))
        tree.links.new(set_overlay.outputs["Result"], set_focus.inputs["User"])
        tree.links.new(on_init.outputs["Out"], flow_input(set_focus))
        tree.links.new(on_init.outputs["Out"], flow_input(screen_anchor))

        result = bpy.ops.node.validate_logic_tree(tree_name=tree.name)
        require(
            result == {"FINISHED"},
            f"Object UI navigation socket tree failed validation: {result}",
        )
    finally:
        bpy.data.node_groups.remove(tree)
        bpy.data.objects.remove(camera, do_unlink=True)
        bpy.data.cameras.remove(camera_data)
        bpy.data.collections.remove(collection)

    print("Object UI navigation socket contracts passed")


def unload_probe_trees():
    names = {"LN Empty Tree", "LN Single Node Tree", "LN Linked Tree"}
    for tree in list(bpy.data.node_groups):
        if tree.name in names:
            bpy.data.node_groups.remove(tree)


def unload_probe_objects():
    names = {LOGIC_NODES_BINDING_OBJECT, LOGIC_NODES_CLEAR_OBJECT}
    for obj in list(bpy.data.objects):
        if obj.name in names:
            bpy.data.objects.remove(obj, do_unlink=True)


def prepare_stale_value_changed_to_socket_label():
    linked_tree = bpy.data.node_groups.get("LN Linked Tree")
    require(linked_tree is not None, "Linked tree is missing before stale-label save probe")
    value_changed_to = find_node(linked_tree, "LogicNativeValueChangedTo")
    require(value_changed_to is not None, "On Value Changed To is missing before stale-label probe")
    value_changed_to.inputs["Target"].name = ""
    require(
        value_changed_to.inputs["Target"].name == "",
        "Could not prepare the legacy empty Target label for save/load migration testing",
    )


def load_probe_trees(blend_path):
    with bpy.data.libraries.load(str(blend_path), link=False) as (data_from, data_to):
        data_to.node_groups = [name for name in data_from.node_groups if name.startswith("LN ")]


def load_probe_objects(blend_path):
    names = {LOGIC_NODES_BINDING_OBJECT, LOGIC_NODES_CLEAR_OBJECT}
    with bpy.data.libraries.load(str(blend_path), link=False) as (data_from, data_to):
        data_to.objects = [name for name in data_from.objects if name in names]


def validate_sound_advanced_tree_compile():
    """Build an editor logic tree with 3D/pause/resume sound nodes and assigned test audio."""
    sound_path = Path(__file__).resolve().parent.parent / "files" / "sound" / "pink_panther.ogg"
    require(sound_path.is_file(), f"Missing acceptance test sound: {sound_path}")

    sound = bpy.data.sounds.load(str(sound_path))
    tree = new_logic_tree("LN Sound Advanced Tree")
    on_init = tree.nodes.new("LogicNativeOnInit")
    play_3d = tree.nodes.new("LogicNativePlaySound3D")
    pause = tree.nodes.new("LogicNativePauseSound")
    resume = tree.nodes.new("LogicNativeResumeSound")

    play_3d.sound = sound
    pause.sound = sound
    resume.sound = sound

    tree.links.new(on_init.outputs["Out"], flow_input(play_3d))
    tree.links.new(play_3d.outputs["Done"], flow_input(pause))
    tree.links.new(pause.outputs["Done"], flow_input(resume))

    require(play_3d.sound == sound, "Play Sound 3D node did not keep the assigned sound")
    require(pause.sound == sound, "Pause Sound node did not keep the assigned sound")
    require(resume.sound == sound, "Resume Sound node did not keep the assigned sound")
    print("Sound advanced logic tree editor compile smoke passed")


def validate_a_tier_editor_hardening():
    tree = new_logic_tree("LN A Tier Hardening Tree")
    try:
        navigate = tree.nodes.new("LogicNativeNavigate")
        set_position = new_set_object_attribute(tree, "WORLD_POSITION")
        next_point = navigate.outputs["Next Point"]
        require(
            next_point.bl_idname == "NodeSocketLogicList",
            "Navigate Next Point output should use the addon list socket style",
        )
        require_color_close(
            next_point.draw_color_simple(),
            LOGIC_SOCKET_COLORS["NodeSocketLogicList"],
            "LogicNativeNavigate.Next Point",
        )
        link = tree.links.new(next_point, set_position.inputs["Value"])
        bpy.context.view_layer.update()
        require(
            link.from_socket == next_point and link.to_socket == set_position.inputs["Value"],
            "Navigate Next Point should link to vector inputs",
        )
        require(getattr(link, "is_valid", True), "Navigate Next Point vector link is invalid")

        follow_path = tree.nodes.new("LogicNativeFollowPath")
        look_at = follow_path.inputs["Look At"]
        dependent_sockets = {
            socket_name: follow_path.inputs[socket_name]
            for socket_name in ("Rot Speed", "Rot Axis", "Front")
        }
        look_at.default_value = False
        bpy.context.view_layer.update()
        for socket_name, socket in dependent_sockets.items():
            require(
                socket.enabled is False,
                f"Follow Path {socket_name} should be disabled when Look At is false",
            )

        look_at.default_value = True
        bpy.context.view_layer.update()
        for socket_name, socket in dependent_sockets.items():
            require(
                socket.enabled is True,
                f"Follow Path {socket_name} should be enabled when Look At is true",
            )
    finally:
        bpy.data.node_groups.remove(tree)

    print("A Tier editor hardening checks passed")


def validate_spatial_analysis_node_ui():
    tree = new_logic_tree("LN Spatial Analysis UI Audit")
    try:
        distance = tree.nodes.new("LogicNativeGetDistance")
        require(distance.bl_label == "Distance Between", "Distance node has the wrong label")
        require(distance.endpoint_a_type == "OBJECT", "Distance A should default to Object")
        require(distance.endpoint_b_type == "OBJECT", "Distance B should default to Object")
        distance_inputs = {socket.identifier: socket for socket in distance.inputs}
        require(
            not distance_inputs["Object A"].is_unavailable
            and distance_inputs["Position A"].is_unavailable,
            "Distance A did not expose only its selected Object socket",
        )
        require(
            not distance_inputs["Object B"].is_unavailable
            and distance_inputs["Position B"].is_unavailable,
            "Distance B did not expose only its selected Object socket",
        )
        distance.endpoint_a_type = "POSITION"
        distance.endpoint_b_type = "POSITION"
        bpy.context.view_layer.update()
        distance_inputs = {socket.identifier: socket for socket in distance.inputs}
        require(
            distance_inputs["Object A"].is_unavailable
            and not distance_inputs["Position A"].is_unavailable,
            "Distance A did not switch to its Position socket",
        )
        require(
            distance_inputs["Object B"].is_unavailable
            and not distance_inputs["Position B"].is_unavailable,
            "Distance B did not switch to its Position socket",
        )

        average = tree.nodes.new("LogicNativeGetAverageCenterPosition")
        require(
            average.bl_label == "Get Average Center Position",
            "Get Average Center Position has the wrong label",
        )
        require(
            average.source_type == "VECTOR_LIST",
            "Get Average Center Position should default to Vector List",
        )
        average_inputs = {socket.identifier: socket for socket in average.inputs}
        require(
            not average_inputs["Vectors"].is_unavailable
            and average_inputs["Objects"].is_unavailable
            and average_inputs["Collection"].is_unavailable,
            "Vector List mode did not expose only Vectors",
        )
        average.source_type = "OBJECT_LIST"
        bpy.context.view_layer.update()
        average_inputs = {socket.identifier: socket for socket in average.inputs}
        require(
            average_inputs["Vectors"].is_unavailable
            and not average_inputs["Objects"].is_unavailable
            and average_inputs["Collection"].is_unavailable,
            "Object List mode did not expose only Objects",
        )
        average.source_type = "COLLECTION"
        bpy.context.view_layer.update()
        average_inputs = {socket.identifier: socket for socket in average.inputs}
        require(
            average_inputs["Vectors"].is_unavailable
            and average_inputs["Objects"].is_unavailable
            and not average_inputs["Collection"].is_unavailable,
            "Collection mode did not expose only Collection",
        )
        require(
            [socket.identifier for socket in average.outputs]
            == ["Center", "Count", "Valid"],
            "Get Average Center Position has the wrong output contract",
        )

        predict = tree.nodes.new("LogicNativePredictMotion")
        require(predict.source_type == "OBJECT", "Predict Motion should default to Object")
        predict_inputs = {socket.identifier: socket for socket in predict.inputs}
        require(
            not predict_inputs["Object"].is_unavailable
            and predict_inputs["Position"].is_unavailable,
            "Predict Motion did not expose only its selected Object source",
        )
        require(
            abs(predict.inputs["Prediction Time"].default_value - 0.5) <= 1.0e-6,
            "Predict Motion has the wrong prediction-time default",
        )
        require(
            abs(predict.inputs["Smoothing Time"].default_value) <= 1.0e-6,
            "Predict Motion should default to unsmoothed velocity",
        )
        predict.source_type = "POSITION"
        bpy.context.view_layer.update()
        predict_inputs = {socket.identifier: socket for socket in predict.inputs}
        require(
            predict_inputs["Object"].is_unavailable
            and not predict_inputs["Position"].is_unavailable,
            "Predict Motion did not switch to its Position source",
        )
    finally:
        bpy.data.node_groups.remove(tree)

    print("Spatial analysis node UI checks passed")


def validate_set_parent_node_ui():
    tree = new_logic_tree("LN Set Parent UI Audit")
    try:
        node = tree.nodes.new("LogicNativeSetParent")
        inputs = {socket.identifier: socket for socket in node.inputs}

        require(node.parent_type == "OBJECT", "Set Parent should default to object parenting")
        require(
            not hasattr(node, "initial_transform"),
            "Set Parent should not expose an initial-transform setting",
        )
        require(
            not hasattr(node, "physics_while_parented"),
            "Set Parent should not expose a physics-policy setting",
        )
        require(inputs["Bone Name"].is_unavailable, "Object parenting should hide Bone Name")
        require(
            not inputs["Compound"].is_unavailable,
            "Modern object parenting should expose Compound",
        )
        require(inputs["Compound"].default_value is False, "Compound should default off")
        require(inputs["Ghost"].is_unavailable, "Modern Set Parent should hide legacy Ghost")

        node.parent_type = "BONE"
        bpy.context.view_layer.update()
        require(
            not inputs["Bone Name"].is_unavailable,
            "Bone parenting should expose Bone Name",
        )
        for socket_name in ("Compound", "Ghost"):
            require(
                inputs[socket_name].is_unavailable,
                f"Modern bone parenting should hide legacy {socket_name}",
            )

        node.parent_type = "OBJECT"
        bpy.context.view_layer.update()
        require(
            inputs["Bone Name"].is_unavailable,
            "Switching back to Object should hide Bone Name",
        )
        require(
            not inputs["Compound"].is_unavailable,
            "Switching back to Object should expose Compound",
        )
    finally:
        bpy.data.node_groups.remove(tree)

    print("Set Parent node UI checks passed")


def validate_dynamic_flow_node_ui():
    tree = new_logic_tree("LN Dynamic Flow UI Audit")
    original_area_type = None
    area = None
    try:
        sequence = tree.nodes.new("LogicNativeSequence")
        require(
            [socket.identifier for socket in sequence.outputs] == ["Then 0", "Then 1"],
            "Sequence should start with two ordered outputs",
        )

        area = bpy.context.window.screen.areas[0]
        original_area_type = area.type
        area.type = "NODE_EDITOR"
        space = area.spaces.active
        space.tree_type = "LogicNodeTree"
        space.node_tree = tree
        tree.nodes.active = sequence
        sequence.select = True
        region = next(region for region in area.regions if region.type == "WINDOW")
        with bpy.context.temp_override(
            window=bpy.context.window,
            screen=bpy.context.window.screen,
            area=area,
            region=region,
        ):
            def activate(node):
                for candidate in tree.nodes:
                    candidate.select = False
                node.select = True
                tree.nodes.active = node

            result = bpy.ops.node.logic_sequence_output_count(delta=1)
            require(result == {"FINISHED"}, "Sequence add-output operator did not finish")
            require(
                [socket.identifier for socket in sequence.outputs]
                == ["Then 0", "Then 1", "Then 2"],
                "Sequence add-output operator did not append Then 2",
            )
            result = bpy.ops.node.logic_sequence_output_count(delta=-1)
            require(result == {"FINISHED"}, "Sequence remove-output operator did not finish")
            require(
                [socket.identifier for socket in sequence.outputs] == ["Then 0", "Then 1"],
                "Sequence remove-output operator did not remove only the final output",
            )
            result = bpy.ops.node.logic_sequence_output_count(delta=-1)
            require(result == {"CANCELLED"}, "Sequence allowed fewer than two outputs")

            integer_switch = tree.nodes.new("LogicNativeSwitchFlowInt")
            require(
                [socket.identifier for socket in integer_switch.inputs] == ["Flow", "Index"],
                "Integer Switch Flow should expose Flow and Index inputs",
            )
            require(
                [socket.identifier for socket in integer_switch.outputs]
                == ["Then 0", "Then 1", "Default"],
                "Integer Switch Flow should start with two cases and Default",
            )
            activate(integer_switch)
            result = bpy.ops.node.logic_switch_flow_int_case_count(delta=1)
            require(result == {"FINISHED"}, "Integer Switch Flow add-case operator did not finish")
            require(
                [socket.identifier for socket in integer_switch.outputs]
                == ["Then 0", "Then 1", "Then 2", "Default"],
                "Integer Switch Flow did not append Then 2 before Default",
            )
            result = bpy.ops.node.logic_switch_flow_int_case_count(delta=-1)
            require(result == {"FINISHED"}, "Integer Switch Flow remove-case operator did not finish")
            result = bpy.ops.node.logic_switch_flow_int_case_count(delta=-1)
            require(result == {"CANCELLED"}, "Integer Switch Flow allowed fewer than two cases")

            string_switch = tree.nodes.new("LogicNativeSwitchFlowString")
            require(
                [socket.identifier for socket in string_switch.inputs]
                == ["Flow", "Value", "Case 0", "Case 1"],
                "String Switch Flow should start with Value and two Case inputs",
            )
            require(
                [socket.identifier for socket in string_switch.outputs]
                == ["Then 0", "Then 1", "Default"],
                "String Switch Flow should start with two cases and Default",
            )
            activate(string_switch)
            result = bpy.ops.node.logic_switch_flow_string_case_count(delta=1)
            require(result == {"FINISHED"}, "String Switch Flow add-case operator did not finish")
            require(
                [socket.identifier for socket in string_switch.inputs]
                == ["Flow", "Value", "Case 0", "Case 1", "Case 2"],
                "String Switch Flow did not append Case 2",
            )
            require(
                [socket.identifier for socket in string_switch.outputs]
                == ["Then 0", "Then 1", "Then 2", "Default"],
                "String Switch Flow did not append Then 2 before Default",
            )
            result = bpy.ops.node.logic_switch_flow_string_case_count(delta=-1)
            require(result == {"FINISHED"}, "String Switch Flow remove-case operator did not finish")
            result = bpy.ops.node.logic_switch_flow_string_case_count(delta=-1)
            require(result == {"CANCELLED"}, "String Switch Flow allowed fewer than two cases")

            router = tree.nodes.new("LogicNativeFlowRouter")
            require(
                [socket.identifier for socket in router.inputs] == ["Flow", "Reset"],
                "Flow Router should expose Flow and Reset inputs",
            )
            require(
                [socket.identifier for socket in router.outputs] == ["Then 0", "Then 1"],
                "Flow Router should start with two ordered outputs",
            )
            activate(router)
            result = bpy.ops.node.logic_flow_router_output_count(delta=1)
            require(result == {"FINISHED"}, "Flow Router add-output operator did not finish")
            require(
                [socket.identifier for socket in router.outputs] == ["Then 0", "Then 1", "Then 2"],
                "Flow Router did not append Then 2",
            )
            result = bpy.ops.node.logic_flow_router_output_count(delta=-1)
            require(result == {"FINISHED"}, "Flow Router remove-output operator did not finish")
            result = bpy.ops.node.logic_flow_router_output_count(delta=-1)
            require(result == {"CANCELLED"}, "Flow Router allowed fewer than two outputs")

            flip_flop = tree.nodes.new("LogicNativeFlipFlop")
            require(
                [socket.identifier for socket in flip_flop.inputs] == ["Flow", "Reset"],
                "Flip-Flop should expose Flow and Reset inputs",
            )
            require(
                [socket.identifier for socket in flip_flop.outputs] == ["A", "B", "Is A"],
                "Flip-Flop should expose A, B, and Is A outputs",
            )

            repeat_limit = tree.nodes.new("LogicNativeRepeatLimit")
            require(
                [socket.identifier for socket in repeat_limit.inputs] == ["Flow", "Reset", "Limit"],
                "Repeat Limit should expose Flow, Reset, and Limit inputs",
            )
            require(
                [socket.identifier for socket in repeat_limit.outputs]
                == ["Accepted", "Blocked", "Remaining", "Is Exhausted"],
                "Repeat Limit should expose both routes and its state outputs",
            )

            wait_for_next_frame = tree.nodes.new("LogicNativeOnNextFrame")
            require(
                wait_for_next_frame.bl_label == "Wait for Next Frame",
                "Wait for Next Frame has the wrong visible label",
            )
            require(
                [socket.identifier for socket in wait_for_next_frame.inputs] == ["Flow"],
                "Wait for Next Frame should expose only its Flow input",
            )
            require(
                [socket.identifier for socket in wait_for_next_frame.outputs] == ["Completed"],
                "Wait for Next Frame should expose only its Completed output",
            )
            require(
                "does not wait for a Logic + Physics step"
                in wait_for_next_frame.bl_description,
                "Wait for Next Frame must document its frame-domain behavior",
            )

            break_loop = tree.nodes.new("LogicNativeBreakLoop")
            require(
                [socket.identifier for socket in break_loop.inputs] == ["Flow"],
                "Break Loop should expose only its execution input",
            )
            require(len(break_loop.outputs) == 0, "Break Loop must not expose an execution output")

            loop = tree.nodes.new("LogicNativeLoop")
            loop_from_list = tree.nodes.new("LogicNativeLoopFromList")
            require(
                [socket.identifier for socket in loop.inputs]
                == ["Flow", "Count", "Ticks to Complete"],
                "Loop should expose its tick distribution input after Count",
            )
            require(
                [socket.identifier for socket in loop_from_list.inputs]
                == ["Flow", "List", "Ticks to Complete"],
                "Loop From List should expose its tick distribution input after List",
            )
            require(
                loop.inputs["Ticks to Complete"].default_value == 1,
                "Loop must default to one tick",
            )
            require(
                loop_from_list.inputs["Ticks to Complete"].default_value == 1,
                "Loop From List must default to one tick",
            )
            require(
                [socket.identifier for socket in loop.outputs]
                == ["Loop", "Completed", "Index"],
                "Loop should expose only its body, completion, and typed index outputs",
            )
            require(
                [socket.identifier for socket in loop_from_list.outputs]
                == ["Loop", "Completed", "Index", "List Item"],
                "Loop From List should expose Completed after its body output",
            )
            require(
                loop_from_list.outputs["List Item"].name == "List Item",
                "Loop From List should expose List Item as its socket name and identifier",
            )

            map_range = tree.nodes.new("LogicNativeMapRange")
            require(map_range.use_clamp, "New Map Range nodes should clamp by default like Blender")

        combine = tree.nodes.new("LogicNativeGate")
        require(combine.inputs["Conditions"].is_multi_input, "And should accept many conditions")
        combine.operation = "NOT"
        require(not combine.inputs["Conditions"].is_multi_input, "Not should accept one condition")
        require(combine.inputs["Conditions"].name == "Condition", "Not input should be singular")
        combine.operation = "XOR"
        require(combine.inputs["Conditions"].is_multi_input, "Xor should accept many conditions")
        require(combine.inputs["Conditions"].name == "Conditions", "Xor input should be plural")
    finally:
        if area is not None and original_area_type is not None:
            area.type = original_area_type
        bpy.data.node_groups.remove(tree)

    print("Dynamic flow node UI checks passed")


def validate_game_property_types(output_dir):
    """Vector, Color, Object, Choice, List and Table game properties: editing, saving, copying."""
    mesh = bpy.data.meshes.new("LN_GamePropertyTypesMesh")
    owner = bpy.data.objects.new("LN_GamePropertyTypesOwner", mesh)
    target = bpy.data.objects.new("LN_GamePropertyTypesTarget", None)
    other = bpy.data.objects.new("LN_GamePropertyTypesOther", mesh)
    for obj in (owner, target, other):
        bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = owner

    def run(operator, **kwargs):
        with bpy.context.temp_override(object=owner, active_object=owner,
                                       selected_objects=[owner, other],
                                       selected_editable_objects=[owner, other]):
            result = getattr(bpy.ops.object, operator)(**kwargs)
        require(result == {'FINISHED'}, f"{operator} failed: {result}")

    def add(type_, name):
        run("game_property_new", type=type_, name=name)
        return owner.game.properties[name]

    def index(name):
        return owner.game.properties.find(name)

    add('VECTOR', "spawn").value = (1.0, 2.0, 3.0)
    add('COLOR', "tint").value = (0.5, 0.25, 1.0, 1.0)
    add('OBJECT', "target").value = target
    mode = add('CHOICE', "mode")
    require([option.name for option in mode.options] == ["A", "B"],
            "a new Choice starts with options A and B")
    mode.value = 'B'
    run("game_property_item_add", index=index("mode"))
    mode.options[-1].name = "Hard"
    mode.active_option_index = 0
    run("game_property_item_remove", index=index("mode"))
    require([option.name for option in mode.options] == ["B", "Hard"] and mode.value == 'B',
            "removing an option keeps the selected one")
    numbers = add('LIST', "numbers")
    numbers.item_type = 'INT'
    for value in (3, 5, 7):
        run("game_property_item_add", index=index("numbers"))
        numbers.elements[-1].value = value
    numbers.active_element_index = 2
    run("game_property_item_move", index=index("numbers"), direction='UP')
    require([e.value for e in numbers.elements] == [3, 7, 5], "list items move")
    table = add('TABLE', "items")
    for name, type_ in (("item", 'STRING'), ("count", 'INT'), ("owner", 'OBJECT')):
        run("game_property_item_add", index=index("items"), target='COLUMN')
        table.columns[-1].name = name
        table.columns[-1].type = type_
    for item, count in (("Apple", 3), ("Sword", 1)):
        run("game_property_item_add", index=index("items"))
        table.rows[-1].cells[0].value = item
        table.rows[-1].cells[1].value = count
    table.rows[0].cells[2].value = target
    try:
        table.columns[0].type = 'LIST'
        require(False, "a column can't be a List")
    except TypeError:
        pass
    table.active_column_index = 1
    run("game_property_item_move", index=index("items"), target='COLUMN', direction='UP')
    require([c.name for c in table.columns] == ["count", "item", "owner"] and
            [cell.value for cell in table.rows[0].cells][:2] == [3, "Apple"],
            "moving a column moves its cells")
    table.active_column_index = 0
    run("game_property_item_remove", index=index("items"), target='COLUMN')
    require([c.name for c in table.columns] == ["item", "owner"] and
            len(table.rows[1].cells) == 2, "removing a column removes its cells")
    table.columns[0].type = 'INT'
    require(table.rows[0].cells[0].value == 0, "a column type change resets its cells")
    table.columns[0].type = 'STRING'
    table.rows[0].cells[0].value = "Apple"
    table.rows[1].cells[0].value = "Sword"

    run("game_property_copy", operation='REPLACE')
    require(other.game.properties["items"].rows[0].cells[1].value == target,
            "copying properties copies Tables")

    path = Path(output_dir) / "logic_nodes_game_property_types.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(path), copy=True)
    bpy.ops.wm.open_mainfile(filepath=str(path))
    owner = bpy.data.objects["LN_GamePropertyTypesOwner"]
    target = bpy.data.objects["LN_GamePropertyTypesTarget"]
    props = owner.game.properties
    require(tuple(props["spawn"].value) == (1.0, 2.0, 3.0), "vector saved")
    require(tuple(round(v, 3) for v in props["tint"].value) == (0.5, 0.25, 1.0, 1.0),
            "color saved")
    require(props["target"].value == target, "object saved")
    require(props["mode"].value == 'B' and len(props["mode"].options) == 2, "choice saved")
    require(props["numbers"].item_type == 'INT' and
            [e.value for e in props["numbers"].elements] == [3, 7, 5], "list saved")
    rows = [[getattr(cell.value, "name", cell.value) for cell in row.cells]
            for row in props["items"].rows]
    require(rows == [["Apple", target.name], ["Sword", None]], f"table saved: {rows}")
    bpy.data.objects.remove(target)
    require(props["target"].value is None and props["items"].rows[0].cells[1].value is None,
            "deleting an object clears references to it")
    print("Game property types checks passed")


def main():
    disable_legacy_logic_addons()
    validate_editor_menus()
    validate_catalog_runtime_parity()
    validate_logic_tree_validation_operator()
    validate_catalog_execution_input_labels()
    validate_value_change_node_ui()
    validate_typecast_node_ui()
    validate_rotation_node_ui()
    validate_object_attribute_node_ui()
    validate_set_physics_state_node_ui()
    validate_logic_socket_palette()
    validate_logic_math_operations()

    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    blend_path = output_dir / "logic_nodes_editor_tree.blend"

    build_probe_trees()
    validate_probe_trees()
    validate_binding_remap_safety()
    validate_sound_advanced_tree_compile()
    validate_a_tier_editor_hardening()
    validate_spatial_analysis_node_ui()
    validate_set_parent_node_ui()
    validate_dynamic_flow_node_ui()
    validate_object_ui_navigation_node_sockets()

    prepare_stale_value_changed_to_socket_label()
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), copy=True)
    unload_probe_trees()
    unload_probe_objects()
    load_probe_trees(blend_path)
    load_probe_objects(blend_path)
    validate_probe_trees()
    validate_game_property_types(output_dir)

    print("LogicNodeTree editor save/load smoke passed")


if __name__ == "__main__":
    main()
