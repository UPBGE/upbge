# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
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


def flow_input(node):
    if "Flow" in node.inputs:
        return node.inputs["Flow"]
    return flow_input(node)


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

EXPECTED_SOCKET_IDNAMES = {
    "LogicNativeOnInit": {
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeOnUpdate": {
        "outputs": {"Out": "NodeSocketLogicExecution"},
    },
    "LogicNativeOnNextFrame": {
        "inputs": {"Flow": "NodeSocketLogicExecution"},
        "outputs": {"Out": "NodeSocketLogicExecution"},
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
    "LogicNativeGate": {
        "inputs": {
            "Condition A": "NodeSocketLogicCondition",
            "Condition B": "NodeSocketLogicCondition",
        },
        "outputs": {"Result": "NodeSocketLogicCondition"},
    },
    "LogicNativeGateList": {
        "inputs": {
            "Condition A": "NodeSocketLogicCondition",
            "Condition B": "NodeSocketLogicCondition",
            "Condition C": "NodeSocketLogicCondition",
            "Condition D": "NodeSocketLogicCondition",
        },
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
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
            "Z": "NodeSocketLogicFloat",
        },
        "outputs": {"Rotation": "NodeSocketLogicRotation"},
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
            "X": "NodeSocketLogicFloat",
            "Y": "NodeSocketLogicFloat",
            "Z": "NodeSocketLogicFloat",
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
    "LogicNativeGetWorldPosition": {"outputs": {"Position": "NodeSocketLogicVector"}},
    "LogicNativeGetLocalPosition": {"outputs": {"Position": "NodeSocketLogicVector"}},
    "LogicNativeGetWorldOrientation": {"outputs": {"Rotation": "NodeSocketLogicRotation"}},
    "LogicNativeGetLocalOrientation": {"outputs": {"Rotation": "NodeSocketLogicRotation"}},
    "LogicNativeGetWorldScale": {"outputs": {"Scale": "NodeSocketLogicVector"}},
    "LogicNativeGetLocalScale": {"outputs": {"Scale": "NodeSocketLogicVector"}},
    "LogicNativeGetLinearVelocity": {"outputs": {"Velocity": "NodeSocketLogicVector"}},
    "LogicNativeGetLocalLinearVelocity": {"outputs": {"Velocity": "NodeSocketLogicVector"}},
    "LogicNativeGetAngularVelocity": {"outputs": {"Velocity": "NodeSocketLogicVector"}},
    "LogicNativeGetLocalAngularVelocity": {"outputs": {"Velocity": "NodeSocketLogicVector"}},
    "LogicNativeGetVisibility": {"outputs": {"Visible": "NodeSocketLogicBool"}},
    "LogicNativeGetObjectColor": {"outputs": {"Color": "NodeSocketLogicColor"}},
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
    "LogicNativeSetWorldPosition": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Position": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLocalPosition": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Position": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetWorldOrientation": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Rotation": "NodeSocketLogicRotation",
        },
    },
    "LogicNativeSetLocalOrientation": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Rotation": "NodeSocketLogicRotation",
        },
    },
    "LogicNativeSetWorldScale": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Scale": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLocalScale": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Scale": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLinearVelocity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Velocity": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLocalLinearVelocity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Velocity": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetAngularVelocity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Velocity": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetLocalAngularVelocity": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Velocity": "NodeSocketLogicVector",
        },
    },
    "LogicNativeApplyImpulse": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Attach": "NodeSocketLogicVector",
            "Impulse": "NodeSocketLogicVector",
        },
    },
    "LogicNativeSetVisibility": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Visible": "NodeSocketLogicBool",
            "Include Children": "NodeSocketLogicBool",
        },
    },
    "LogicNativeSetObjectColor": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Color": "NodeSocketLogicColor",
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
        "rna_enums": {"input_type": {"TAP", "DOWN", "UP"}},
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
        "inputs": {"Object": "NodeSocketLogicObject"},
        "outputs": {
            "On Enter": "NodeSocketLogicExecution",
            "On Over": "NodeSocketLogicExecution",
            "On Exit": "NodeSocketLogicExecution",
            "Entered": "NodeSocketLogicCondition",
            "Is Over": "NodeSocketLogicCondition",
            "Exited": "NodeSocketLogicCondition",
            "Point": "NodeSocketLogicVector",
            "Normal": "NodeSocketLogicVector",
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
    "LogicNativeModifyPropertyClamped": {
        "inputs": {
            "Flow": "NodeSocketLogicExecution",
            "Object": "NodeSocketLogicObject",
            "Property": "NodeSocketLogicString",
            "Value": "NodeSocketLogicFloat",
            "Min": "NodeSocketLogicFloat",
            "Max": "NodeSocketLogicFloat",
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
                "LogicNativeOnUpdate",
                "LogicNativeOnNextFrame",
                "LogicNativeValueChangedTo",
                "LogicNativeValueChanged",
                "LogicNativeBooleanEdge",
                "LogicNativeOnCollision",
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
            ],
            "separators": 1,
        },
        "Input": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Input"]),
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
                "LogicNativeEuler",
                "LogicNativeVectorToRotation",
                "LogicNativeSeparateXY",
                "LogicNativeSeparateEuler",
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
        "Values/Properties": {
            "nodes": [
                "LogicNativeHasProperty",
                "LogicNativeEvaluateProperty",
                "LogicNativeGetGamePropertyFloat",
                "LogicNativeGetGamePropertyInt",
                "LogicNativeGetGamePropertyBool",
                "LogicNativeGetGamePropertyString",
                "LogicNativeGetTreeProperty",
                "LogicNativeGetGlobalProperty",
                "LogicNativeListGlobalProperties",
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
                "LogicNativeModifyPropertyClamped",
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
        "Animation/Get Bone Data": {
            "nodes": [
                "LogicNativeGetBoneHeadWorld",
                "LogicNativeGetBoneTailWorld",
                "LogicNativeGetBoneLength",
                "LogicNativeGetBoneCenterWorld",
                "LogicNativeGetBoneHeadPoseWorld",
                "LogicNativeGetBoneTailPoseWorld",
                "LogicNativeGetBoneCenterPoseWorld",
                "LogicNativeGetBoneAttribute",
            ],
            "separators": 2,
        },
        "Animation/Set Bone Data": {
            "nodes": [
                "LogicNativeSetBonePoseLocation",
                "LogicNativeSetBonePoseRotation",
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
            "nodes": [],
        },
        "Nodes/Groups": {
            "nodes": [
                "LogicNativeRunLogicTree",
                "LogicNativeInstallLogicTree",
                "LogicNativeStartLogicTree",
                "LogicNativeStopLogicTree",
            ],
        },
        "Objects": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Objects"]),
            "separators": 1,
        },
        "Objects/Get Attribute": {
            "nodes": [
                "LogicNativeGetWorldPosition",
                "LogicNativeGetWorldOrientation",
                "LogicNativeGetLocalPosition",
                "LogicNativeGetLocalOrientation",
                "LogicNativeGetWorldScale",
                "LogicNativeGetLocalScale",
                "LogicNativeGetVisibility",
                "LogicNativeGetObjectColor",
                "LogicNativeGetLinearVelocity",
                "LogicNativeGetLocalLinearVelocity",
                "LogicNativeGetAngularVelocity",
                "LogicNativeGetLocalAngularVelocity",
                "LogicNativeGetObjectAttribute",
                "LogicNativeGetObjectID",
                "LogicNativeGetAxisVector",
            ],
            "separators": 1,
        },
        "Objects/Set Attribute": {
            "nodes": [
                "LogicNativeSetObjectAttribute",
                "LogicNativeSetWorldPosition",
                "LogicNativeSetWorldOrientation",
                "LogicNativeSetLocalPosition",
                "LogicNativeSetLocalOrientation",
                "LogicNativeSetWorldScale",
                "LogicNativeSetLocalScale",
                "LogicNativeSetVisibility",
                "LogicNativeSetObjectColor",
                "LogicNativeSetLinearVelocity",
                "LogicNativeSetLocalLinearVelocity",
                "LogicNativeSetAngularVelocity",
                "LogicNativeSetLocalAngularVelocity",
            ],
            "separators": 1,
        },
        "Objects/Transformation": {
            "nodes": [
                "LogicNativeApplyMovement",
                "LogicNativeApplyRotation",
                "LogicNativeApplyForce",
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
                "LogicNativeAddObject",
                "LogicNativeSetParent",
                "LogicNativeGetParent",
                "LogicNativeGetChild",
                "LogicNativeGetChildByName",
                "LogicNativeRemoveParent",
                "LogicNativeSpawnPool",
                "LogicNativeRemoveObject",
                "LogicNativeReplaceMesh",
            ],
            "separators": 1,
        },
        "Scene": {
            "paths": list(logic_menu_module.LOGIC_IMPORTANT_SUBMENU_PATHS["Scene"]),
            "nodes": [
                "LogicNativeGetTimescale",
                "LogicNativeSetTimescale",
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
                "LogicNativeGate",
                "LogicNativeGateList",
                "LogicNativeNone",
                "LogicNativeNotNone",
                "LogicNativeValueValid",
                "LogicNativeCollision",
                "LogicNativeObjectsColliding",
            ],
            "separators": 2,
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
        "Time": {
            "nodes": [
                "LogicNativeTime",
                "LogicNativeDeltaFactor",
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
                "LogicNativeProjectileRay",
                "LogicNativeAddPhysicsConstraint",
                "LogicNativeRemovePhysicsConstraint",
                "LogicNativeObjectsColliding",
                "LogicNativeSetPhysics",
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
        unsupported = tree.nodes.new("LogicNativeSetCustomCursor")
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

    clear_object = get_or_create_cube(LOGIC_NODES_CLEAR_OBJECT)
    set_active_object(clear_object)
    result = bpy.ops.object.logic_nodes_binding_add()
    require(result == {"FINISHED"}, f"Failed to add clear-test binding: {result}")
    result = bpy.ops.object.logic_nodes_binding_clear()
    require(result == {"FINISHED"}, f"Failed to clear Logic Nodes binding: {result}")
    require(
        len(clear_object.game.logic_node_bindings) == 0,
        "Logic Nodes binding clear left collection entries behind",
    )


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
    require(hasattr(bpy.types, "LogicNodeTree"), "LogicNodeTree RNA type is not registered")

    empty_tree = new_logic_tree("LN Empty Tree")
    require(empty_tree.bl_idname == "LogicNodeTree", "Empty tree has wrong RNA type")

    single_node_tree = new_logic_tree("LN Single Node Tree")
    init_node = single_node_tree.nodes.new("LogicNativeOnInit")
    init_node.name = "On Init Probe"

    linked_tree = new_logic_tree("LN Linked Tree")
    on_update = linked_tree.nodes.new("LogicNativeOnUpdate")
    branch = linked_tree.nodes.new("LogicNativeBranch")
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
    threshold = linked_tree.nodes.new("LogicNativeThreshold")
    ranged_threshold = linked_tree.nodes.new("LogicNativeRangedThreshold")
    within_range = linked_tree.nodes.new("LogicNativeWithinRange")
    math = linked_tree.nodes.new("LogicNativeMath")
    target_position = linked_tree.nodes.new("LogicNativeValueVector")
    get_position = linked_tree.nodes.new("LogicNativeGetWorldPosition")
    get_local_position = linked_tree.nodes.new("LogicNativeGetLocalPosition")
    get_world_orientation = linked_tree.nodes.new("LogicNativeGetWorldOrientation")
    get_local_orientation = linked_tree.nodes.new("LogicNativeGetLocalOrientation")
    get_world_scale = linked_tree.nodes.new("LogicNativeGetWorldScale")
    get_local_scale = linked_tree.nodes.new("LogicNativeGetLocalScale")
    get_visibility = linked_tree.nodes.new("LogicNativeGetVisibility")
    get_object_color = linked_tree.nodes.new("LogicNativeGetObjectColor")
    get_light_color = linked_tree.nodes.new("LogicNativeGetLightColor")
    get_light_power = linked_tree.nodes.new("LogicNativeGetLightPower")
    get_velocity = linked_tree.nodes.new("LogicNativeGetLinearVelocity")
    get_local_velocity = linked_tree.nodes.new("LogicNativeGetLocalLinearVelocity")
    get_angular_velocity = linked_tree.nodes.new("LogicNativeGetAngularVelocity")
    get_local_angular_velocity = linked_tree.nodes.new("LogicNativeGetLocalAngularVelocity")
    get_prop_float = linked_tree.nodes.new("LogicNativeGetGamePropertyFloat")
    get_prop_bool = linked_tree.nodes.new("LogicNativeGetGamePropertyBool")
    get_prop_int = linked_tree.nodes.new("LogicNativeGetGamePropertyInt")
    get_prop_string = linked_tree.nodes.new("LogicNativeGetGamePropertyString")
    vector_math = linked_tree.nodes.new("LogicNativeVectorMath")
    set_position = linked_tree.nodes.new("LogicNativeSetWorldPosition")
    set_local_position = linked_tree.nodes.new("LogicNativeSetLocalPosition")
    set_world_orientation = linked_tree.nodes.new("LogicNativeSetWorldOrientation")
    set_local_orientation = linked_tree.nodes.new("LogicNativeSetLocalOrientation")
    set_world_scale = linked_tree.nodes.new("LogicNativeSetWorldScale")
    set_local_scale = linked_tree.nodes.new("LogicNativeSetLocalScale")
    set_visibility = linked_tree.nodes.new("LogicNativeSetVisibility")
    set_object_color = linked_tree.nodes.new("LogicNativeSetObjectColor")
    set_light_color = linked_tree.nodes.new("LogicNativeSetLightColor")
    set_light_power = linked_tree.nodes.new("LogicNativeSetLightPower")
    set_light_shadow = linked_tree.nodes.new("LogicNativeSetLightShadow")
    make_light_unique = linked_tree.nodes.new("LogicNativeMakeLightUnique")
    set_velocity = linked_tree.nodes.new("LogicNativeSetLinearVelocity")
    set_local_velocity = linked_tree.nodes.new("LogicNativeSetLocalLinearVelocity")
    set_angular_velocity = linked_tree.nodes.new("LogicNativeSetAngularVelocity")
    set_local_angular_velocity = linked_tree.nodes.new("LogicNativeSetLocalAngularVelocity")
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
    gate_list = linked_tree.nodes.new("LogicNativeGateList")
    once = linked_tree.nodes.new("LogicNativeOnce")
    boolean_edge = linked_tree.nodes.new("LogicNativeBooleanEdge")
    value_changed = linked_tree.nodes.new("LogicNativeValueChanged")
    value_changed_to = linked_tree.nodes.new("LogicNativeValueChangedTo")
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
    modify_property_clamped = linked_tree.nodes.new("LogicNativeModifyPropertyClamped")
    keyboard_key = linked_tree.nodes.new("LogicNativeKeyboardKey")
    mouse_button = linked_tree.nodes.new("LogicNativeMouseButton")
    add_object = linked_tree.nodes.new("LogicNativeAddObject")
    set_parent = linked_tree.nodes.new("LogicNativeSetParent")
    remove_parent = linked_tree.nodes.new("LogicNativeRemoveParent")
    remove_object = linked_tree.nodes.new("LogicNativeRemoveObject")
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
    gate_list.location = (820.0, -40.0)
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
    modify_property_clamped.location = (1080.0, -1000.0)
    keyboard_key.location = (1340.0, 120.0)
    mouse_button.location = (1340.0, -40.0)
    add_object.location = (1340.0, -200.0)
    set_parent.location = (1340.0, -360.0)
    remove_parent.location = (1340.0, -520.0)
    remove_object.location = (1340.0, -680.0)
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
    gate_list.operation = "AND"
    has_property.inputs["Name"].default_value = "enabled"
    toggle_property.inputs["Property"].default_value = "enabled"
    modify_property.inputs["Property"].default_value = "speed"
    modify_property_clamped.inputs["Property"].default_value = "speed"
    keyboard_key.inputs["Key"].default_value = "A"
    mouse_button.inputs["Button"].default_value = "LEFTMOUSE"
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

    linked_tree.links.new(on_update.outputs["Out"], branch.inputs["Flow"])
    linked_tree.links.new(value_output(condition), gate.inputs["Condition A"])
    linked_tree.links.new(value_output(condition), gate.inputs["Condition B"])
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
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_movement))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_rotation))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_force))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_torque))
    linked_tree.links.new(branch.outputs["True"], flow_input(apply_impulse))
    linked_tree.links.new(branch.outputs["True"], flow_input(set_prop_int))
    linked_tree.links.new(get_position.outputs["Position"], vector_math.inputs["A"])
    linked_tree.links.new(value_output(target_position), vector_math.inputs["B"])
    linked_tree.links.new(value_output(target_position), set_local_position.inputs["Position"])
    linked_tree.links.new(euler.outputs["Rotation"], set_world_orientation.inputs["Rotation"])
    linked_tree.links.new(
        get_local_orientation.outputs["Rotation"], set_local_orientation.inputs["Rotation"]
    )
    linked_tree.links.new(get_world_scale.outputs["Scale"], set_world_scale.inputs["Scale"])
    linked_tree.links.new(get_local_scale.outputs["Scale"], set_local_scale.inputs["Scale"])
    linked_tree.links.new(get_visibility.outputs["Visible"], set_visibility.inputs["Visible"])
    linked_tree.links.new(color_rgba.outputs["Color"], set_object_color.inputs["Color"])
    linked_tree.links.new(get_light_color.outputs["Color"], set_light_color.inputs["Color"])
    linked_tree.links.new(get_light_power.outputs["Power"], set_light_power.inputs["Power"])
    linked_tree.links.new(combine_xy.outputs["Vector"], separate_xy.inputs["Vector"])
    linked_tree.links.new(combine_xyz.outputs["Vector"], separate_xyz.inputs["Vector"])
    linked_tree.links.new(vector_math.outputs["Result"], set_position.inputs["Position"])
    linked_tree.links.new(get_velocity.outputs["Velocity"], set_velocity.inputs["Velocity"])
    linked_tree.links.new(get_local_velocity.outputs["Velocity"], set_local_velocity.inputs["Velocity"])
    linked_tree.links.new(get_angular_velocity.outputs["Velocity"], set_angular_velocity.inputs["Velocity"])
    linked_tree.links.new(
        get_local_angular_velocity.outputs["Velocity"],
        set_local_angular_velocity.inputs["Velocity"],
    )
    linked_tree.links.new(value_output(target_position), vector_input(apply_movement, "Vector", "Movement"))
    linked_tree.links.new(get_world_orientation.outputs["Rotation"], apply_rotation.inputs["Rotation"])
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

    build_binding_probe(linked_tree)


def find_node(tree, bl_idname):
    for node in tree.nodes:
        if node.bl_idname == bl_idname:
            return node
    return None


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
    require(len(linked_tree.nodes) == 120, "Linked tree should keep all probe nodes")
    require(len(linked_tree.links) == 62, "Linked tree should keep all probe links")

    validate_probe_tree_socket_idnames(single_node_tree)
    validate_probe_tree_socket_idnames(linked_tree)

    math = find_node(linked_tree, "LogicNativeMath")
    require(math is not None, "Math node did not survive save/load")
    require(math.operation == "MAXIMUM", "Math node lost operation")
    vector_math = find_node(linked_tree, "LogicNativeVectorMath")
    require(vector_math is not None, "Vector Math node did not survive save/load")
    require(vector_math.operation == "MAXIMUM", "Vector Math node lost operation")
    gate = find_node(linked_tree, "LogicNativeGate")
    require(gate is not None, "Gate node did not survive save/load")
    require(gate.operation == "AND", "Gate node lost operation")
    euler = find_node(linked_tree, "LogicNativeEuler")
    require(euler is not None, "Euler node did not survive save/load")
    require(abs(euler.inputs["Z"].default_value - 0.3) <= 0.0001, "Euler node lost input")
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
    require(
        find_node(linked_tree, "LogicNativeGetGamePropertyString") is not None,
        "Get Game Property String node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLocalPosition") is not None,
        "Get Local Position node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetWorldOrientation") is not None,
        "Get World Orientation node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLocalOrientation") is not None,
        "Get Local Orientation node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetWorldScale") is not None,
        "Get World Scale node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLocalScale") is not None,
        "Get Local Scale node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetAngularVelocity") is not None,
        "Get Angular Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLocalLinearVelocity") is not None,
        "Get Local Linear Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetLocalAngularVelocity") is not None,
        "Get Local Angular Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetVisibility") is not None,
        "Get Visibility node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeGetObjectColor") is not None,
        "Get Color node did not survive save/load",
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
    require(
        find_node(linked_tree, "LogicNativeSetLocalPosition") is not None,
        "Set Local Position node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetWorldOrientation") is not None,
        "Set World Orientation node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLocalOrientation") is not None,
        "Set Local Orientation node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetWorldScale") is not None,
        "Set World Scale node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLocalScale") is not None,
        "Set Local Scale node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetAngularVelocity") is not None,
        "Set Angular Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLocalLinearVelocity") is not None,
        "Set Local Linear Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetLocalAngularVelocity") is not None,
        "Set Local Angular Velocity node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetVisibility") is not None,
        "Set Visibility node did not survive save/load",
    )
    require(
        find_node(linked_tree, "LogicNativeSetObjectColor") is not None,
        "Set Color node did not survive save/load",
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

    binding_object = bpy.data.objects.get(LOGIC_NODES_BINDING_OBJECT)
    require(binding_object is not None, "Binding probe object did not survive save/load")
    require(
        len(binding_object.game.logic_node_bindings) == 1,
        "Logic node binding collection was not persisted",
    )
    binding = binding_object.game.logic_node_bindings[0]
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
        expected = EXPECTED_SOCKET_IDNAMES.get(node.bl_idname)
        if expected is None:
            continue
        for collection_name in ("inputs", "outputs"):
            expected_sockets = expected.get(collection_name, {})
            sockets = getattr(node, collection_name)
            for socket_name, expected_idname in expected_sockets.items():
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
        set_position = tree.nodes.new("LogicNativeSetWorldPosition")
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
        link = tree.links.new(next_point, set_position.inputs["Position"])
        bpy.context.view_layer.update()
        require(
            link.from_socket == next_point and link.to_socket == set_position.inputs["Position"],
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


def main():
    disable_legacy_logic_addons()
    validate_editor_menus()
    validate_catalog_runtime_parity()
    validate_logic_tree_validation_operator()
    validate_catalog_execution_input_labels()
    validate_logic_socket_palette()
    validate_logic_math_operations()

    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    blend_path = output_dir / "logic_nodes_editor_tree.blend"

    build_probe_trees()
    validate_probe_trees()
    validate_sound_advanced_tree_compile()
    validate_a_tier_editor_hardening()

    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), copy=True)
    unload_probe_trees()
    unload_probe_objects()
    load_probe_trees(blend_path)
    load_probe_objects(blend_path)
    validate_probe_trees()

    print("LogicNodeTree editor save/load smoke passed")


if __name__ == "__main__":
    main()
