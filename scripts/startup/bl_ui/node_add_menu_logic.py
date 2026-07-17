# SPDX-FileCopyrightText: 2026 UPBGE Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.app.translations import (
    contexts as i18n_contexts,
)
from bpy.types import Menu

from bl_ui import node_add_menu
from bl_ui.node_add_menu_logic_catalog import (
    LOGIC_NODE_DEFERRED_CATALOG,
    LOGIC_NODE_PARITY_CATALOG,
    LOGIC_NODE_STATUS_IMPLEMENTED,
    catalog_native_idnames,
    catalog_status_for_idname,
    deferred_native_idnames,
)


def _menu_item(path):
    return {
        "kind": "menu",
        "path": path,
    }


def _node_item(
        node_idname,
        label=None,
        *,
        searchable_enum=None,
        search_weight=0.0,
        status=None,
):
    if status is None:
        status = catalog_status_for_idname(node_idname)

    return {
        "kind": "node",
        "idname": node_idname,
        "label": label,
        "searchable_enum": searchable_enum,
        "search_weight": search_weight,
        "status": status,
    }


def _separator():
    return {
        "kind": "separator",
    }


def _is_visible_node_item(item):
    node_idname = item.get("idname", "")
    if not node_idname.startswith("LogicNative"):
        return True
    return item.get("status") == LOGIC_NODE_STATUS_IMPLEMENTED


LOGIC_ROOT_MENU_GROUPS = (
    ("Events", "Game", "Input", "Values"),
    ("Animation", "Lights", "Nodes", "Objects", "Scene", "Sound"),
    ("Logic", "Math", "Physics", "Python", "Time"),
    ("Data", "File", "Network", "Portals"),
    ("Render", "UI"),
    ("Layout", "Utility"),
)

LOGIC_ROOT_MENU_PATHS = tuple(path for group in LOGIC_ROOT_MENU_GROUPS for path in group)

LOGIC_IMPORTANT_SUBMENU_PATHS = {
    "Input": ("Input/Mouse", "Input/Keyboard", "Input/Gamepad", "Input/VR"),
    "Values": ("Values/Vector", "Values/Properties"),
    "Animation": (
        "Animation/Actions",
        "Animation/Get Rest Bone Data",
        "Animation/Get Pose Bone Data",
        "Animation/Set Pose Bone Data",
        "Animation/Advanced Bone Data",
        "Animation/Bone Constraints",
    ),
    "Nodes": ("Nodes/Materials", "Nodes/Geometry", "Nodes/Groups"),
    "Objects": (
        "Objects/Get Attribute",
        "Objects/Set Attribute",
        "Objects/Transformation",
        "Objects/Object Data",
        "Objects/Curves",
    ),
    "Scene": ("Scene/Camera", "Scene/Post FX", "Scene/Collections"),
    "Sound": ("Sound/FMOD",),
    "Events": ("Events/Custom",),
    "Logic": ("Logic/Trees", "Logic/Bricks", "Logic/Loops"),
    "Physics": ("Physics/Vehicle", "Physics/Character"),
    "Data": ("Data/List", "Data/Dict", "Data/Variables", "Data/Path"),
    "UI": ("UI/Widgets",),
}


def _build_root_items():
    items = []
    for group_index, group in enumerate(LOGIC_ROOT_MENU_GROUPS):
        if group_index:
            items.append(_separator())
        items.extend(_menu_item(path) for path in group)
    return tuple(items)


LOGIC_MENU_LAYOUTS = {
    "Root": _build_root_items(),
    "Events": (
        _node_item("LogicNativeOnInit", "On Init"),
        _node_item("LogicNativeOnUpdate", "On Update"),
        _node_item("LogicNativeOnNextFrame", "On Next Frame"),
        _node_item("LogicNativeValueChangedTo", "On Value Changed To"),
        _node_item("LogicNativeValueChanged", "On Value Changed"),
        _node_item("LogicNativeBooleanEdge", "Boolean Edge"),
        _node_item("LogicNativeOnCollision", "On Collision"),
        _node_item("LogicNativeOnce", "Do Once"),
        _separator(),
        _menu_item("Events/Custom"),
    ),
    "Events/Custom": (
        _node_item("LogicNativeSendEvent", "Send Event"),
        _node_item("LogicNativeReceiveEvent", "Receive Event"),
    ),
    "Game": (
        _node_item("LogicNativeLoadBlendFile", "Load Blender File"),
        _node_item("LogicNativeQuitGame", "Quit Game"),
        _node_item("LogicNativeRestartGame", "Restart Game"),
        _separator(),
        _node_item("LogicNativeSaveGame", "Save Game"),
        _node_item("LogicNativeLoadGame", "Load Game"),
    ),
    "Input": (
        _menu_item("Input/Mouse"),
        _menu_item("Input/Keyboard"),
        _menu_item("Input/Gamepad"),
        _menu_item("Input/VR"),
    ),
    "Input/Mouse": (
        _node_item("LogicNativeMouseButton", "Mouse Button"),
        _node_item("LogicNativeMouseWheel", "Mouse Wheel"),
        _node_item("LogicNativeMouseMoved", "Mouse Moved"),
        _node_item("LogicNativeMouseOver", "Mouse Over"),
        _node_item("LogicNativeMouseRay", "Mouse Ray"),
        _separator(),
        _node_item("LogicNativeSetCursorVisibility", "Cursor Visibility"),
        _node_item("LogicNativeCursorPosition", "Cursor Position"),
        _node_item("LogicNativeSetCursorPosition", "Set Cursor Position"),
        _node_item("LogicNativeCursorMovement", "Cursor Movement"),
        _separator(),
        _node_item("LogicNativeMouseLook", "Mouse Look"),
    ),
    "Input/Keyboard": (
        _node_item("LogicNativeKeyboardKey", "Keyboard Key"),
        _node_item("LogicNativeKeyboardActive", "Keyboard Active"),
        _separator(),
        _node_item("LogicNativeKeyCode", "Key Code"),
        _node_item("LogicNativeKeyLogger", "Key Logger"),
    ),
    "Input/Gamepad": (
        _node_item("LogicNativeGamepadButton", "Gamepad Button"),
        _node_item("LogicNativeGamepadStick", "Gamepad Sticks"),
        _node_item("LogicNativeGamepadActive", "Gamepad Active"),
        _separator(),
        _node_item("LogicNativeGamepadVibration", "Gamepad Vibrate"),
        _node_item("LogicNativeGamepadLook", "Gamepad Look"),
    ),
    "Input/VR": (),
    "Values": (
        _node_item("LogicNativeValueBool", "Boolean"),
        _node_item("LogicNativeValueInt", "Integer"),
        _node_item("LogicNativeValueFloat", "Float"),
        _node_item("LogicNativeValueString", "String"),
        _node_item("LogicNativeStringOperation", searchable_enum="operation"),
        _node_item("LogicNativeFormattedString", "Formatted String"),
        _node_item("LogicNativeRandomValue", "Random Value"),
        _node_item("LogicNativeRandomFloat", "Random Float"),
        _node_item("LogicNativeRandomInt", "Random Integer"),
        _node_item("LogicNativeTypecast", "Typecast Value"),
        _node_item("LogicNativeValueSwitch", "Value Switch"),
        _node_item("LogicNativeValueSwitchList", "Value Switch List"),
        _node_item("LogicNativeValueSwitchListCompare", "Value Switch List Compare"),
        _node_item("LogicNativeStoreValue", "Store Value"),
        _separator(),
        _menu_item("Values/Vector"),
        _menu_item("Values/Properties"),
        _separator(),
        _node_item("LogicNativeInvertValue", "Invert"),
        _node_item("LogicNativeLimitRange", "Limit Range"),
    ),
    "Values/Vector": (
        _node_item("LogicNativeValueColor", "Color"),
        _node_item("LogicNativeColorRGB", "Color RGB"),
        _node_item("LogicNativeColorRGBA", "Color RGBA"),
        _node_item("LogicNativeValueVector", "Vector"),
        _node_item("LogicNativeEuler", "Euler"),
        _node_item("LogicNativeVectorToRotation", "Vector to Rotation"),
        _separator(),
        _node_item("LogicNativeSeparateXY", "Separate XY"),
        _node_item("LogicNativeSeparateEuler", "Separate Euler"),
        _node_item("LogicNativeSeparateXYZ", "Separate XYZ"),
        _node_item("LogicNativeCombineXY", "Combine XY"),
        _node_item("LogicNativeCombineXYZ", "Combine XYZ"),
        _node_item("LogicNativeCombineXYZW", "Combine XYZW"),
        _node_item("LogicNativeRandomVector", "Random Vector"),
        _node_item("LogicNativeResizeVector", "Resize Vector"),
        _node_item("LogicNativeXYZToMatrix", "XYZ to Matrix"),
        _node_item("LogicNativeMatrixToXYZ", "Matrix to XYZ"),
        _node_item("LogicNativeVectorRotate", "Vector Rotate"),
    ),
    "Values/Properties": (
        _node_item("LogicNativeHasProperty", "Has Property"),
        _node_item("LogicNativeEvaluateProperty", "Evaluate Object Property"),
        _separator(),
        _node_item("LogicNativeGetGamePropertyFloat", "Get Object Property (Float)"),
        _node_item("LogicNativeGetGamePropertyInt", "Get Object Property (Integer)"),
        _node_item("LogicNativeGetGamePropertyBool", "Get Object Property (Boolean)"),
        _node_item("LogicNativeGetGamePropertyString", "Get Object Property (String)"),
        _node_item("LogicNativeGetTreeProperty", "Get Tree Property"),
        _node_item("LogicNativeGetGlobalProperty", "Get Global Property"),
        _node_item("LogicNativeListGlobalProperties", "List Global Properties"),
        _separator(),
        _node_item("LogicNativeSetGamePropertyFloat", "Set Object Property (Float)"),
        _node_item("LogicNativeSetGamePropertyInt", "Set Object Property (Integer)"),
        _node_item("LogicNativeSetGamePropertyBool", "Set Object Property (Boolean)"),
        _node_item("LogicNativeSetGamePropertyString", "Set Object Property (String)"),
        _node_item("LogicNativeSetTreeProperty", "Set Tree Property"),
        _node_item("LogicNativeToggleTreeProperty", "Toggle Tree Property"),
        _node_item("LogicNativeSetGlobalProperty", "Set Global Property"),
        _separator(),
        _node_item("LogicNativeLoadVariable", "Load Variable"),
        _node_item("LogicNativeSaveVariable", "Save Variable"),
        _node_item("LogicNativeLoadVariableDict", "Load Variable Dict"),
        _node_item("LogicNativeSaveVariableDict", "Save Variable Dict"),
        _node_item("LogicNativeClearVariables", "Clear Variables"),
        _node_item("LogicNativeListSavedVariables", "List Saved Variables"),
        _node_item("LogicNativeRemoveVariable", "Remove Variable"),
        _separator(),
        _node_item("LogicNativeToggleProperty", "Toggle Property"),
        _node_item("LogicNativeModifyProperty", "Modify Property"),
        _node_item("LogicNativeModifyPropertyClamped", "Modify Property Clamped"),
        _separator(),
        _node_item("LogicNativeCopyProperty", "Copy Property"),
    ),
    "Animation": (
        _menu_item("Animation/Actions"),
        _menu_item("Animation/Get Rest Bone Data"),
        _menu_item("Animation/Get Pose Bone Data"),
        _menu_item("Animation/Set Pose Bone Data"),
        _menu_item("Animation/Advanced Bone Data"),
        _menu_item("Animation/Bone Constraints"),
    ),
    "Animation/Actions": (
        _node_item("LogicNativePlayAction", "Play Action"),
        _node_item("LogicNativeStopAction", "Stop Action"),
        _node_item("LogicNativeSetActionFrame", "Set Action Frame"),
        _node_item("LogicNativeActionDone", "Action Done"),
        _separator(),
        _node_item("LogicNativeAnimationStatus", "Animation Status"),
    ),
    "Animation/Get Rest Bone Data": (
        _node_item("LogicNativeGetBoneHeadWorld", "Get Rest Bone Head"),
        _node_item("LogicNativeGetBoneTailWorld", "Get Rest Bone Tail"),
        _node_item("LogicNativeGetBoneLength", "Get Rest Bone Length"),
        _node_item("LogicNativeGetBoneCenterWorld", "Get Rest Bone Center"),
    ),
    "Animation/Get Pose Bone Data": (
        _node_item("LogicNativeGetBoneHeadPoseWorld", "Get Pose Bone Head"),
        _node_item("LogicNativeGetBoneTailPoseWorld", "Get Pose Bone Tail"),
        _node_item("LogicNativeGetBoneCenterPoseWorld", "Get Pose Bone Center"),
        _node_item("LogicNativeGetBonePoseRotation", "Get Pose Bone Rotation"),
        _node_item("LogicNativeGetBonePoseScale", "Get Pose Bone Scale"),
        _node_item("LogicNativeGetBonePoseTransform", "Get Pose Bone Transform"),
    ),
    "Animation/Advanced Bone Data": (
        _node_item("LogicNativeGetBoneAttribute", "Get Bone Attribute"),
        _node_item("LogicNativeSetBoneAttribute", "Set Bone Attribute"),
    ),
    "Animation/Set Pose Bone Data": (
        _node_item("LogicNativeSetBonePoseLocation", "Set Pose Bone Location"),
        _node_item("LogicNativeSetBonePoseRotation", "Set Pose Bone Rotation"),
        _node_item("LogicNativeSetBonePoseScale", "Set Pose Bone Scale"),
        _node_item("LogicNativeSetBonePoseTransform", "Set Pose Bone Transform"),
    ),
    "Animation/Bone Constraints": (
        _node_item("LogicNativeSetBoneConstraintInfluence", "Set Constraint Influence"),
        _node_item("LogicNativeSetBoneConstraintTarget", "Set Constraint Target"),
        _node_item("LogicNativeSetBoneConstraintAttribute", "Set Constraint Attribute"),
    ),
    "Lights": (
        _node_item("LogicNativeGetLightColor", "Get Light Color"),
        _node_item("LogicNativeGetLightPower", "Get Light Power"),
        _separator(),
        _node_item("LogicNativeMakeLightUnique", "Make Light Unique"),
        _node_item("LogicNativeSetLightColor", "Set Light Color"),
        _node_item("LogicNativeSetLightPower", "Set Light Power"),
        _node_item("LogicNativeSetLightShadow", "Set Light Shadow"),
    ),
    "Nodes": (
        _menu_item("Nodes/Materials"),
        _menu_item("Nodes/Geometry"),
        _menu_item("Nodes/Groups"),
        _separator(),
        _node_item("LogicNativeGetEditorNodeValue", "Get Editor Node Value"),
        _node_item("LogicNativeSetEditorNodeValue", "Set Editor Node Value"),
        _node_item("LogicNativeMakeNodeTreeUnique", "Make Node Tree Unique"),
        _node_item("LogicNativeSetNodeMute", "Set Node Mute"),
    ),
    "Nodes/Materials": (
        _node_item("LogicNativeGetMaterialFromSlot", "Get Material From Slot"),
        _node_item("LogicNativeGetMaterialSlotCount", "Get Material Slot Count"),
        _node_item("LogicNativeGetMaterialName", "Get Material Name"),
        _node_item("LogicNativeGetMaterialParameter", "Get Material Parameter"),
        _separator(),
        _node_item("LogicNativeSetMaterialSlot", "Assign Material To Slot"),
        _node_item("LogicNativeSetMaterialParameter", "Set Material Parameter"),
        _node_item("LogicNativePlayMaterialSequence", "Play Material Sequence"),
    ),
    "Nodes/Geometry": (
        _node_item("LogicNativeSetGeometryNodesInput", "Set Geometry Nodes Input"),
        _node_item("LogicNativeEnableDisableModifier", "Enable or Disable Modifier"),
        _node_item("LogicNativeAssignGeometryNodesModifier", "Assign Geometry Nodes Modifier"),
    ),
    "Nodes/Groups": (
        _node_item("LogicNativeGetNodeGroupSocketValue", "Get Node Group Socket"),
        _node_item("LogicNativeSetNodeGroupSocketValue", "Set Node Group Socket"),
    ),
    "Objects": (
        _menu_item("Objects/Get Attribute"),
        _menu_item("Objects/Set Attribute"),
        _separator(),
        _menu_item("Objects/Transformation"),
        _menu_item("Objects/Object Data"),
        _menu_item("Objects/Curves"),
    ),
    "Objects/Get Attribute": (
        _node_item("LogicNativeGetObjectAttribute", "Get Object Attribute"),
        _separator(),
        _node_item("LogicNativeGetObjectID", "Get Object ID"),
        _node_item("LogicNativeGetAxisVector", "Get Axis Vector"),
    ),
    "Objects/Set Attribute": (
        _node_item("LogicNativeSetObjectAttribute", "Set Attribute"),
    ),
    "Objects/Transformation": (
        _node_item("LogicNativeApplyMovement", "Apply Movement"),
        _node_item("LogicNativeApplyRotation", "Apply Rotation"),
        _node_item("LogicNativeApplyForce", "Apply Force"),
        _node_item("LogicNativeApplyForceToTarget", "Apply Force To Target"),
        _node_item("LogicNativeApplyTorque", "Apply Torque"),
        _node_item("LogicNativeApplyImpulse", "Apply Impulse"),
        _separator(),
        _node_item("LogicNativeMoveToward", "Move To"),
        _node_item("LogicNativeNavigate", "Navigate"),
        _node_item("LogicNativeFollowPath", "Follow Path"),
        _node_item("LogicNativeTranslate", "Translate"),
        _node_item("LogicNativeRotateToward", "Rotate To"),
        _node_item("LogicNativeSlowFollow", "Slow Follow"),
        _node_item("LogicNativeAlignAxisToVector", "Align Axis to Vector"),
    ),
    "Objects/Object Data": (
        _node_item("LogicNativeFindObject", "Get Object"),
        _node_item("LogicNativeObjectByName", "Get Object By Name"),
        _node_item("LogicNativeGetOwner", "Get Owner"),
        _node_item("LogicNativeGetDistance", "Get Distance"),
        _node_item("LogicNativeGetGroupCenterPosition", "Get Group Center position"),
        _node_item("LogicNativeAddObject", "Add Object"),
        _node_item("LogicNativeSetParent", "Set Parent"),
        _node_item("LogicNativeGetParent", "Get Parent"),
        _node_item("LogicNativeGetChild", "Get Child"),
        _node_item("LogicNativeGetChildByName", "Get Child By Name"),
        _node_item("LogicNativeRemoveParent", "Remove Parent"),
        _node_item("LogicNativeSpawnPool", "Spawn Pool"),
        _node_item("LogicNativeRemoveObject", "Remove Object"),
        _separator(),
        _node_item("LogicNativeReplaceMesh", "Replace Mesh"),
    ),
    "Objects/Curves": (
        _node_item("LogicNativeEvaluateCurve", "Evaluate Curve"),
    ),
    "Scene": (
        _menu_item("Scene/Camera"),
        _menu_item("Scene/Post FX"),
        _menu_item("Scene/Collections"),
        _separator(),
        _node_item("LogicNativeGetTimescale", "Get Timescale"),
        _node_item("LogicNativeSetTimescale", "Set Timescale"),
        _separator(),
        _node_item("LogicNativeLoadScene", "Load Scene"),
        _node_item("LogicNativeSetScene", "Set Scene"),
        _node_item("LogicNativeGetScene", "Get Scene"),
    ),
    "Scene/Camera": (
        _node_item("LogicNativeGetActiveCamera", "Active Camera"),
        _node_item("LogicNativeCameraRay", "Camera Ray"),
        _node_item("LogicNativeSetCamera", "Set Camera"),
        _node_item("LogicNativeSetCameraFov", "Set FOV"),
        _node_item("LogicNativeSetCameraOrthoScale", "Set Orthographic Scale"),
        _node_item("LogicNativeWorldToScreen", "World To Screen"),
        _node_item("LogicNativeScreenToWorld", "Screen To World"),
    ),
    "Scene/Post FX": (),
    "Scene/Collections": (
        _node_item("LogicNativeGetCollection", "Get Collection"),
        _node_item("LogicNativeGetCollectionObjects", "Get Collection Objects"),
        _node_item("LogicNativeGetCollectionObjectNames", "Get Collection Object Names"),
        _node_item("LogicNativeSetCollectionVisibility", "Set Collection Visibility"),
        _node_item("LogicNativeSetOverlayCollection", "Set Overlay Collection"),
        _node_item("LogicNativeRemoveOverlayCollection", "Remove Overlay Collection"),
    ),
    "Sound": (
        _menu_item("Sound/FMOD"),
        _separator(),
        _node_item("LogicNativePlaySound", "Play Sound"),
        _node_item("LogicNativePlaySound3D", "Play Sound 3D"),
        _node_item("LogicNativeStartSpeaker", "Start Speaker"),
        _node_item("LogicNativePauseSound", "Pause Sound"),
        _node_item("LogicNativeResumeSound", "Resume Sound"),
        _node_item("LogicNativeStopSound", "Stop Sound"),
        _node_item("LogicNativeStopAllSounds", "Stop All Sounds"),
        _separator(),
        _node_item("LogicNativeGetSound", "Get Sound"),
    ),
    "Sound/FMOD": (),
    "Logic": (
        _menu_item("Logic/Trees"),
        _menu_item("Logic/Bricks"),
        _menu_item("Logic/Loops"),
        _separator(),
        _node_item("LogicNativeBranch", "Branch"),
        _node_item("LogicNativeGate", searchable_enum="operation"),
        _node_item("LogicNativeGateList", searchable_enum="operation"),
        _node_item("LogicNativeNone", "None"),
        _node_item("LogicNativeNotNone", "Not None"),
        _node_item("LogicNativeValueValid", "Value Valid"),
        _separator(),
        _node_item("LogicNativeCollision", "Is Colliding"),
        _node_item("LogicNativeObjectsColliding", "Objects Colliding"),
    ),
    "Logic/Trees": (
        _node_item("LogicNativeStartLogicTree", "Start Logic Tree"),
        _node_item("LogicNativeStopLogicTree", "Stop Logic Tree"),
        _node_item("LogicNativeRunLogicTree", "Run Logic Tree"),
        _node_item("LogicNativeInstallLogicTree", "Install Logic Tree"),
        _node_item("LogicNativeLogicTreeStatus", "Logic Tree Status"),
    ),
    "Logic/Bricks": (),
    "Logic/Loops": (
        _node_item("LogicNativeLoop", "Loop"),
    ),
    "Math": (
        _node_item("LogicNativeMath", searchable_enum="operation"),
        _node_item("LogicNativeFormula", "Formula"),
        _node_item("LogicNativeVectorMath", searchable_enum="operation"),
        _separator(),
        _node_item("LogicNativeClampValue", "Clamp"),
        _node_item("LogicNativeCompare", searchable_enum="operation"),
        _node_item("LogicNativeMapRange", "Map Range"),
        _separator(),
        _node_item("LogicNativeThreshold", searchable_enum="operation"),
        _node_item("LogicNativeRangedThreshold", searchable_enum="operation"),
        _node_item("LogicNativeWithinRange", searchable_enum="operation"),
    ),
    "Physics": (
        _node_item("LogicNativeGetCollisionGroup", "Get Collision Layers"),
        _node_item("LogicNativeSetCollisionGroup", "Set Collision Layers"),
        _separator(),
        _node_item("LogicNativeGetGravity", "Get Gravity"),
        _node_item("LogicNativeSetGravity", "Set Gravity"),
        _node_item("LogicNativeRaycast", "Raycast"),
        _node_item("LogicNativeRaycastAll", "Raycast All"),
        _node_item("LogicNativeShapeCast", "Shape Cast"),
        _node_item("LogicNativeShapeCastAll", "Shape Cast All"),
        _node_item("LogicNativeProjectileRay", "Projectile Ray"),
        _node_item("LogicNativeAddPhysicsConstraint", "Add Rigid Body Constraints"),
        _node_item("LogicNativeGetRigidBodyConstraints", "Get Rigid Body Constraints"),
        _node_item("LogicNativeRemovePhysicsConstraint", "Remove Rigid Body Constraints"),
        _node_item("LogicNativeObjectsColliding", "Objects Colliding"),
        _node_item("LogicNativeSetPhysics", "Enable Physics Body"),
        _node_item("LogicNativeSetDynamics", "Set Dynamics"),
        _node_item("LogicNativeRebuildCollisionShape", "Rebuild Collision Shape"),
        _node_item("LogicNativeGetRigidBodyAttribute", "Get Rigid Body Attribute"),
        _node_item("LogicNativeSetRigidBodyAttribute", "Set Rigid Body Attribute"),
        _separator(),
        _menu_item("Physics/Vehicle"),
        _menu_item("Physics/Character"),
    ),
    "Physics/Vehicle": (
        _node_item("LogicNativeVehicleControl", "Vehicle Control"),
        _node_item("LogicNativeVehicleAccelerate", "Accelerate"),
        _node_item("LogicNativeVehicleBrake", "Brake"),
        _node_item("LogicNativeVehicleSetAttributes", "Set Vehicle Attributes"),
        _node_item("LogicNativeVehicleSteer", "Steer"),
    ),
    "Physics/Character": (
        _node_item("LogicNativeSetCharacterWalkDirection", "Walk"),
        _node_item("LogicNativeCharacterJump", "Jump"),
        _node_item("LogicNativeGetCharacterInfo", "Get Physics Info"),
        _node_item("LogicNativeSetCharacterJumpSpeed", "Set Jump Force"),
        _node_item("LogicNativeSetCharacterMaxJumps", "Set Max Jumps"),
        _node_item("LogicNativeSetCharacterGravity", "Set Gravity"),
        _node_item("LogicNativeSetCharacterVelocity", "Set Velocity"),
    ),
    # Taxonomy placeholder only — Python addon nodes (BGELogicTree) are not available here.
    "Python": (),
    "Time": (
        _node_item("LogicNativeTime", "Time"),
        _node_item("LogicNativeDeltaFactor", "Delta Factor"),
        _separator(),
        _node_item("LogicNativeDelay", "Delay"),
        _node_item("LogicNativeTimer", "Timer"),
        _node_item("LogicNativeCooldown", "Cooldown"),
        _node_item("LogicNativePulsify", "Pulsify"),
        _node_item("LogicNativeBarrier", "Barrier"),
        _node_item("LogicNativeTweenValue", "Tween Value"),
    ),
    "Data": (
        _menu_item("Data/List"),
        _menu_item("Data/Dict"),
        _menu_item("Data/Variables"),
        _menu_item("Data/Path"),
    ),
    "Data/List": (
        _node_item("LogicNativeLoopFromList", "Loop From List"),
        _node_item("LogicNativeEmptyList", "New List"),
        _node_item("LogicNativeListLength", "List Length"),
        _node_item("LogicNativeListGetItem", "List Get Item"),
        _node_item("LogicNativeMakeList", "Make List"),
        _node_item("LogicNativeListFromItems", "List From Items"),
        _node_item("LogicNativeListExtend", "Extend List"),
        _node_item("LogicNativeListDuplicate", "Duplicate List"),
        _node_item("LogicNativeListContains", "List Contains"),
        _separator(),
        _node_item("LogicNativeListAppend", "Append"),
        _node_item("LogicNativeListRemoveIndex", "Remove Index"),
        _node_item("LogicNativeListRemoveValue", "Remove Value"),
        _node_item("LogicNativeListSetIndex", "Set List Index"),
        _node_item("LogicNativeListRandomItem", "Get Random List Item"),
    ),
    "Data/Dict": (
        _node_item("LogicNativeEmptyDict", "New Dictionary"),
        _node_item("LogicNativeDictGetKey", "Get Dictionary Key"),
        _node_item("LogicNativeDictHasKey", "Dictionary Has Key"),
        _node_item("LogicNativeDictGetKeys", "Get Dictionary Keys"),
        _node_item("LogicNativeMakeDict", "Make Dictionary"),
        _node_item("LogicNativeDictLength", "Dictionary Length"),
        _node_item("LogicNativeDictMerge", "Merge Dictionaries"),
        _separator(),
        _node_item("LogicNativeDictSetKey", "Set Dictionary Key"),
        _node_item("LogicNativeDictRemoveKey", "Remove Dictionary Key"),
    ),
    "Data/Variables": (),
    "Data/Path": (
        _node_item("LogicNativeFilePath", "File Path"),
        _node_item("LogicNativeJoinPath", "Join Path"),
        _node_item("LogicNativeGetMasterFolder", "Get Master Folder"),
    ),
    "File": (
        _node_item("LogicNativeLoadFileContent", "Load File Content"),
        _separator(),
        _node_item("LogicNativeGetImage", "Get Image"),
    ),
    "Network": (),
    "Portals": (),
    "Render": (
        _node_item("LogicNativeGetFullscreen", "Get Fullscreen"),
        _node_item("LogicNativeSetFullscreen", "Set Fullscreen"),
        _node_item("LogicNativeGetResolution", "Get Resolution"),
        _node_item("LogicNativeSetResolution", "Set Resolution"),
        _node_item("LogicNativeGetVSync", "Get VSync"),
        _node_item("LogicNativeSetVSync", "Set VSync", searchable_enum="vsync_mode"),
        _separator(),
        _node_item("LogicNativeShowFramerate", "Show Framerate"),
        _node_item("LogicNativeShowProfile", "Show Profile"),
        _separator(),
        _node_item("LogicNativeDrawLine", "Draw Line"),
        _node_item("LogicNativeDrawCube", "Draw Cube"),
        _node_item("LogicNativeDrawBox", "Draw Box"),
        _node_item("LogicNativeDraw", "Draw"),
    ),
    "UI": (
        _menu_item("UI/Widgets"),
    ),
    "UI/Widgets": (
        _node_item("LogicNativeGetFont", "Get Font"),
        _node_item("LogicNativeSetCustomCursor", "Set Custom Cursor"),
    ),
    "Layout": (
        _node_item("NodeReroute", "Reroute"),
        _node_item("NodeFrame", "Frame", search_weight=-1.0),
    ),
    "Utility": (
        _node_item("LogicNativePrint", "Print"),
    ),
}


def iter_visible_node_items():
    def walk(menu_path):
        for item in LOGIC_MENU_LAYOUTS.get(menu_path, ()):
            if item["kind"] == "node":
                if _is_visible_node_item(item):
                    yield item
            elif item["kind"] == "menu":
                yield from walk(item["path"])

    yield from walk("Root")


def _sanitize_path(menu_path):
    return menu_path.lower().replace("/", "_").replace(" ", "_")


def _draw_menu_items(menu, context, items):
    pending_separator = False
    drew_item = False

    for item in items:
        kind = item["kind"]
        if kind == "separator":
            if drew_item:
                pending_separator = True
            continue

        if pending_separator:
            menu.layout.separator()
            pending_separator = False

        if kind == "menu":
            menu.draw_menu(menu.layout, item["path"])
        elif kind == "node":
            if not _is_visible_node_item(item):
                continue
            searchable_enum = item.get("searchable_enum")
            if searchable_enum:
                menu.node_operator_with_searchable_enum(
                    context,
                    menu.layout,
                    item["idname"],
                    searchable_enum,
                    item.get("search_weight", 0.0),
                )
            else:
                menu.node_operator(
                    menu.layout,
                    item["idname"],
                    label=item.get("label"),
                    search_weight=item.get("search_weight", 0.0),
                )

        drew_item = True


def _make_menu_base(menu_path):
    label = "" if menu_path == "Root" else menu_path.rsplit("/", 1)[-1]

    def draw(self, context):
        _draw_menu_items(self, context, LOGIC_MENU_LAYOUTS[menu_path])

    return type(
        f"NODE_MT_logic_node_{_sanitize_path(menu_path)}_base",
        (Menu,),
        {
            "bl_label": label,
            "bl_translation_context": i18n_contexts.operator_default,
            "menu_path": menu_path,
            "draw": draw,
        },
    )


def _menu_idname(kind, menu_path):
    if menu_path == "Root":
        return f"NODE_MT_logic_node_{kind}_all"
    return f"NODE_MT_logic_node_{kind}_{_sanitize_path(menu_path)}"


MENU_BASES = {menu_path: _make_menu_base(menu_path) for menu_path in LOGIC_MENU_LAYOUTS}

add_menus = node_add_menu.generate_menus(
    {
        _menu_idname("add", menu_path): menu_base
        for menu_path, menu_base in MENU_BASES.items()
    },
    template=node_add_menu.AddNodeMenu,
    base_dict={},
)

swap_menus = node_add_menu.generate_menus(
    {
        _menu_idname("swap", menu_path): menu_base
        for menu_path, menu_base in MENU_BASES.items()
    },
    template=node_add_menu.SwapNodeMenu,
    base_dict={},
)


classes = (
    *add_menus,
    *swap_menus,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
