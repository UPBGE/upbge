/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Merged Logic Nodes (2026-11-07): one node with a dropdown standing for several older nodes that
 * do the same job. Each option *is* an older node:
 * - the editor declares the older node's sockets, draws its buttons and runs its init and update
 *   (node_logic_merged.cc), so the sockets have the older node's identifiers, names, types and
 *   defaults;
 * - the game compiles the node with the older node's definition (LN_NodeRegistry::
 *   FindNodeDefinitionForNode), so an option compiles to exactly the older node's program.
 * The option is kept in bNode::custom4 (no Logic Node uses custom3 or custom4), so custom1 and
 * custom2 keep the older node's own settings. Since 2026-11-08 the older nodes are no node types
 * of their own (no RNA struct, not in Blender's registry: the files were converted): each lives
 * on as its merged node's option (node_logic_<merged>_options.cc, logic_option_register), and
 * the game keeps its definition under its former idname.
 */

#pragma once

#include <algorithm>
#include <cstddef>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "DNA_node_types.h"

#include "NOD_logic_descriptors.hh"

namespace blender::bke {
struct bNodeType;
}

namespace blender::nodes::logic {

struct MergedNodeOption {
  /** RNA enum identifier of the option. */
  const char *identifier;
  const char *name;
  const char *description;
  /** The older node this option is. */
  const char *idname;
};

struct MergedNode {
  const char *idname;
  const char *ui_name;
  const char *ui_description;
  /** Label of the option dropdown ("Type", "Device"...). */
  const char *option_label;
  Span<MergedNodeOption> options;
  /** Outputs after the older node's own, the same for every option. */
  Span<PinMetadata> extra_outputs;
};

namespace merged_detail {

template<size_t Size>
constexpr Span<MergedNodeOption> options(const MergedNodeOption (&items)[Size])
{
  return Span<MergedNodeOption>(items, int64_t(Size));
}

inline constexpr MergedNodeOption get_property_options[] = {
    {"FLOAT", "Float", "A number (formerly Get Game Property Float)", "LogicNativeGetGamePropertyFloat"},
    {"INT", "Integer", "A whole number (formerly Get Game Property Int)", "LogicNativeGetGamePropertyInt"},
    {"BOOLEAN", "Boolean", "True or false (formerly Get Game Property Bool)", "LogicNativeGetGamePropertyBool"},
    {"STRING", "String", "Text (formerly Get Game Property String)", "LogicNativeGetGamePropertyString"},
    {"ANY",
     "Any",
     "Any value: lists, tables, objects, vectors; the name may be linked (formerly Get Object "
     "Property)",
     "LogicNativeGetGameProperty"},
};
inline constexpr PinMetadata get_property_extra_outputs[] = {
    {"Exists", "Exists", PinType::Condition, "false", false},
};
inline constexpr MergedNodeOption set_property_options[] = {
    {"FLOAT", "Float", "Write a number (formerly Set Game Property Float)", "LogicNativeSetGamePropertyFloat"},
    {"INT", "Integer", "Write a whole number (formerly Set Game Property Int)", "LogicNativeSetGamePropertyInt"},
    {"BOOLEAN", "Boolean", "Write true or false (formerly Set Game Property Bool)", "LogicNativeSetGamePropertyBool"},
    {"STRING", "String", "Write text (formerly Set Game Property String)", "LogicNativeSetGamePropertyString"},
    {"ANY",
     "Any",
     "Write any value: lists, tables, objects, vectors (formerly Set Object Property)",
     "LogicNativeSetGameProperty"},
};
inline constexpr MergedNodeOption modify_property_options[] = {
    {"MODIFY", "Modify", "Change a number by Value (formerly Modify Property)", "LogicNativeModifyProperty"},
    {"TOGGLE", "Toggle", "Flip true and false (formerly Toggle Property)", "LogicNativeToggleProperty"},
};
inline constexpr MergedNodeOption switch_flow_options[] = {
    {"INT", "Integer", "Run the output whose number is Index (formerly Switch Flow (Integer))", "LogicNativeSwitchFlowInt"},
    {"STRING", "String", "Run the output whose Case text is Value (formerly Switch Flow (String))", "LogicNativeSwitchFlowString"},
};
inline constexpr MergedNodeOption logic_tree_options[] = {
    {"START", "Start", "Start the object's tree (formerly Start Logic Tree)", "LogicNativeStartLogicTree"},
    {"STOP", "Stop", "Stop the object's tree (formerly Stop Logic Tree)", "LogicNativeStopLogicTree"},
    {"RUN",
     "Run Once",
     "Run the tree once in the next Logic + Physics step (formerly Run Logic Tree)",
     "LogicNativeRunLogicTree"},
    {"INSTALL", "Install", "Add a tree to the object (formerly Install Logic Tree)", "LogicNativeInstallLogicTree"},
};
inline constexpr MergedNodeOption value_changed_options[] = {
    {"ANY", "Any Change", "Fire when the value changes (formerly On Value Changed)", "LogicNativeValueChanged"},
    {"TO_VALUE",
     "To a Value",
     "Fire when the value changes to Target, or a number crosses it (formerly On Value Changed To)",
     "LogicNativeValueChangedTo"},
};
inline constexpr MergedNodeOption key_button_options[] = {
    {"KEYBOARD", "Keyboard", "A keyboard key (formerly Keyboard Key)", "LogicNativeKeyboardKey"},
    {"MOUSE", "Mouse", "A mouse button (formerly Mouse Button)", "LogicNativeMouseButton"},
    {"GAMEPAD", "Gamepad", "A gamepad button (formerly Gamepad Button)", "LogicNativeGamepadButton"},
};
inline constexpr MergedNodeOption raycast_options[] = {
    {"NEAREST", "Nearest", "The nearest hit (formerly Raycast)", "LogicNativeRaycast"},
    {"ALL", "All", "Every hit along the ray, as lists (formerly Raycast All)", "LogicNativeRaycastAll"},
};
inline constexpr MergedNodeOption shape_cast_options[] = {
    {"NEAREST", "Nearest", "The nearest hit (formerly Shape Cast)", "LogicNativeShapeCast"},
    {"ALL", "All", "Every hit along the cast, as lists (formerly Shape Cast All)", "LogicNativeShapeCastAll"},
};
inline constexpr MergedNodeOption force_options[] = {
    {"FORCE", "Force", "Push the body every step (formerly Apply Force)", "LogicNativeApplyForce"},
    {"TORQUE", "Torque", "Turn the body every step (formerly Apply Torque)", "LogicNativeApplyTorque"},
    {"IMPULSE", "Impulse", "One push at an attach point (formerly Apply Impulse)", "LogicNativeApplyImpulse"},
};
inline constexpr MergedNodeOption movement_options[] = {
    {"MOVE", "Move", "Move by Vector (formerly Apply Movement)", "LogicNativeApplyMovement"},
    {"MOVE_BY_SPEED",
     "Move by Speed (Translate)",
     "Move by Vector x Speed x the step's time (formerly Translate)",
     "LogicNativeTranslate"},
    {"ROTATE", "Rotate", "Turn by Rotation (formerly Apply Rotation)", "LogicNativeApplyRotation"},
};
inline constexpr MergedNodeOption vehicle_options[] = {
    {"ACCELERATE", "Accelerate", "Drive the wheels (formerly Accelerate)", "LogicNativeVehicleAccelerate"},
    {"BRAKE", "Brake", "Brake the wheels (formerly Brake)", "LogicNativeVehicleBrake"},
    {"STEER", "Steer", "Steer the wheels (formerly Steer)", "LogicNativeVehicleSteer"},
};
inline constexpr MergedNodeOption character_options[] = {
    {"GRAVITY", "Gravity", "The character's gravity (formerly character Set Gravity)", "LogicNativeSetCharacterGravity"},
    {"JUMP_FORCE", "Jump Force", "How high it jumps (formerly Set Jump Force)", "LogicNativeSetCharacterJumpSpeed"},
    {"MAX_JUMPS", "Max Jumps", "Jumps before landing (formerly Set Max Jumps)", "LogicNativeSetCharacterMaxJumps"},
};
inline constexpr MergedNodeOption set_light_options[] = {
    {"COLOR", "Color", "The light's color (formerly Set Light Color)", "LogicNativeSetLightColor"},
    {"POWER", "Power", "The light's power (formerly Set Light Power)", "LogicNativeSetLightPower"},
    {"SHADOW", "Shadow", "Whether it casts shadows (formerly Set Light Shadow)", "LogicNativeSetLightShadow"},
};
inline constexpr MergedNodeOption get_light_options[] = {
    {"COLOR", "Color", "The light's color (formerly Get Light Color)", "LogicNativeGetLightColor"},
    {"POWER", "Power", "The light's power (formerly Get Light Power)", "LogicNativeGetLightPower"},
};
inline constexpr MergedNodeOption camera_lens_options[] = {
    {"FOV", "Field of View", "A perspective camera's angle (formerly Set FOV)", "LogicNativeSetCameraFov"},
    {"ORTHO_SCALE",
     "Orthographic Scale",
     "An orthographic camera's size (formerly Set Orthographic Scale)",
     "LogicNativeSetCameraOrthoScale"},
};
inline constexpr MergedNodeOption debug_overlay_options[] = {
    {"FRAMERATE", "Framerate", "The frame rate overlay (formerly Show Framerate)", "LogicNativeShowFramerate"},
    {"PROFILE", "Profile", "The profile overlay (formerly Show Profile)", "LogicNativeShowProfile"},
};
inline constexpr MergedNodeOption sound_options[] = {
    {"PAUSE", "Pause", "Pause the sound (formerly Pause Sound)", "LogicNativePauseSound"},
    {"RESUME", "Resume", "Resume the sound (formerly Resume Sound)", "LogicNativeResumeSound"},
    {"STOP", "Stop", "Stop the sound (formerly Stop Sound)", "LogicNativeStopSound"},
    {"STOP_ALL",
     "Stop All",
     "Stop every sound, Python and logic-brick sounds too (formerly Stop All Sounds)",
     "LogicNativeStopAllSounds"},
};
inline constexpr MergedNodeOption paused_options[] = {
    {"GAME", "Game", "Pause logic time and physics (formerly Set Game Paused)", "LogicNativeSetGamePaused"},
    {"PHYSICS", "Physics Only", "Pause physics, logic keeps running (formerly Set Physics Paused)", "LogicNativeSetPhysicsPaused"},
};
inline constexpr MergedNodeOption scene_options[] = {
    {"REPLACE", "Replace", "Replace this scene by another (formerly Set Scene)", "LogicNativeSetScene"},
    {"ADD", "Add to This Scene", "Merge another scene into this one (formerly Load Scene)", "LogicNativeLoadScene"},
};
inline constexpr MergedNodeOption overlay_options[] = {
    {"SHOW", "Show", "Show a collection on top of a camera's view (formerly Set Overlay Collection)", "LogicNativeSetOverlayCollection"},
    {"REMOVE", "Remove", "Remove an overlay collection (formerly Remove Overlay Collection)", "LogicNativeRemoveOverlayCollection"},
};
inline constexpr MergedNodeOption child_options[] = {
    {"INDEX", "Index", "The child at a place (formerly Get Child)", "LogicNativeGetChild"},
    {"NAME", "Name", "The child with a name (formerly Get Child By Name)", "LogicNativeGetChildByName"},
};
inline constexpr MergedNodeOption is_none_options[] = {
    {"IS_NONE", "Is None", "True when the value is None (formerly None)", "LogicNativeNone"},
    {"NOT_NONE", "Is Not None", "True when the value is not None (formerly Not None, Value Valid)", "LogicNativeNotNone"},
};
inline constexpr MergedNodeOption load_variable_options[] = {
    {"ONE", "One", "One saved variable (formerly Load Variable)", "LogicNativeLoadVariable"},
    {"ALL", "All (Dictionary)", "Every saved variable as a dictionary (formerly Load Variable Dict)", "LogicNativeLoadVariableDict"},
};
inline constexpr MergedNodeOption save_variable_options[] = {
    {"ONE", "One", "Save one variable (formerly Save Variable)", "LogicNativeSaveVariable"},
    {"ALL", "All (Dictionary)", "Save a dictionary as the whole file (formerly Save Variable Dict)", "LogicNativeSaveVariableDict"},
};
inline constexpr MergedNodeOption remove_variable_options[] = {
    {"ONE", "One", "Remove one variable (formerly Remove Variable)", "LogicNativeRemoveVariable"},
    {"ALL", "All", "Remove every variable of the file (formerly Clear Variables)", "LogicNativeClearVariables"},
};
inline constexpr MergedNodeOption bone_point_options[] = {
    {"POSE_HEAD", "Pose Head", "The posed bone's head (formerly Get Pose Bone Head)", "LogicNativeGetBoneHeadPoseWorld"},
    {"POSE_TAIL", "Pose Tail", "The posed bone's tail (formerly Get Pose Bone Tail)", "LogicNativeGetBoneTailPoseWorld"},
    {"POSE_CENTER", "Pose Center", "The posed bone's middle (formerly Get Pose Bone Center)", "LogicNativeGetBoneCenterPoseWorld"},
    {"REST_HEAD", "Rest Head", "The bone's head at rest, in world space (formerly Get Rest Bone Head)", "LogicNativeGetBoneHeadWorld"},
    {"REST_TAIL", "Rest Tail", "The bone's tail at rest, in world space (formerly Get Rest Bone Tail)", "LogicNativeGetBoneTailWorld"},
    {"REST_CENTER", "Rest Center", "The bone's middle at rest, in world space (formerly Get Rest Bone Center)", "LogicNativeGetBoneCenterWorld"},
    {"REST_LENGTH", "Rest Length", "The bone's length (formerly Get Rest Bone Length)", "LogicNativeGetBoneLength"},
};
inline constexpr MergedNodeOption bone_constraint_options[] = {
    {"INFLUENCE", "Influence", "The constraint's influence (formerly Set Constraint Influence)", "LogicNativeSetBoneConstraintInfluence"},
    {"TARGET", "Target", "The constraint's target object (formerly Set Target)", "LogicNativeSetBoneConstraintTarget"},
    {"OTHER", "Other Setting", "Any setting of the constraint by name (formerly Set Attribute)", "LogicNativeSetBoneConstraintAttribute"},
};
inline constexpr MergedNodeOption cursor_options[] = {
    {"POSITION", "Position", "Where the cursor is, 0 to 1 (formerly Cursor Position)", "LogicNativeCursorPosition"},
    {"MOVEMENT", "Movement", "How far the cursor moved, in pixels (formerly Cursor Movement)", "LogicNativeCursorMovement"},
};
inline constexpr MergedNodeOption display_options[] = {
    {"FULLSCREEN", "Fullscreen", "Whether the window is fullscreen (formerly Get Fullscreen)", "LogicNativeGetFullscreen"},
    {"RESOLUTION", "Resolution", "The window's size (formerly Get Resolution)", "LogicNativeGetResolution"},
    {"VSYNC", "VSync", "The vertical sync mode (formerly Get VSync)", "LogicNativeGetVSync"},
};
/* Threshold's options are shown as one "Pass When" dropdown with the older nodes' own operation
 * (custom1): Above / Below are Threshold, Within / Outside are Ranged Threshold. */
inline constexpr MergedNodeOption threshold_options[] = {
    {"LIMIT", "Above or Below", "Today's Threshold", "LogicNativeThreshold"},
    {"RANGE", "Within or Outside", "Today's Ranged Threshold", "LogicNativeRangedThreshold"},
};
inline constexpr MergedNodeOption delay_options[] = {
    {"QUEUE",
     "Queue (Delay)",
     "Every Flow gets its own countdown (formerly Delay)",
     "LogicNativeDelay"},
    {"RESTART",
     "Restart (Timer)",
     "A new Flow starts the countdown again; only the latest counts (formerly Timer)",
     "LogicNativeTimer"},
};

}  // namespace merged_detail

inline constexpr MergedNode all_merged_nodes[] = {
    {"LogicNativeGetGamePropertyValue", "Get Game Property",
     "Read a game property of the owner or another object; Exists tells whether it is there",
     "Type", merged_detail::options(merged_detail::get_property_options),
     detail::pin_span(merged_detail::get_property_extra_outputs)},
    {"LogicNativeSetGamePropertyValue", "Set Game Property",
     "Write a game property of the owner or another object, at the end of the step", "Type",
     merged_detail::options(merged_detail::set_property_options), {}},
    {"LogicNativeModifyGameProperty", "Modify Property",
     "Change a number property by a value (Add, Multiply...), or flip a true / false one (Toggle)",
     "Operation", merged_detail::options(merged_detail::modify_property_options), {}},
    {"LogicNativeSwitchFlow", "Switch Flow",
     "Run the output that matches a number or a text", "Type",
     merged_detail::options(merged_detail::switch_flow_options), {}},
    {"LogicNativeControlLogicTree", "Control Logic Tree",
     "Start, stop, run once or install a logic tree on an object", "Action",
     merged_detail::options(merged_detail::logic_tree_options), {}},
    {"LogicNativeOnValueChange", "On Value Changed",
     "Fire when a value changes, or when it changes to a target value", "Mode",
     merged_detail::options(merged_detail::value_changed_options), {}},
    {"LogicNativeKeyButton", "Key / Button",
     "A keyboard key, a mouse button or a gamepad button: pressed, held or released", "Device",
     merged_detail::options(merged_detail::key_button_options), {}},
    {"LogicNativeCastRay", "Raycast", "Cast a ray and report the nearest hit or every hit",
     "Hits", merged_detail::options(merged_detail::raycast_options), {}},
    {"LogicNativeCastShape", "Shape Cast",
     "Sweep a shape and report the nearest hit or every hit", "Hits",
     merged_detail::options(merged_detail::shape_cast_options), {}},
    {"LogicNativeApplyPhysicsForce", "Apply Force",
     "Push or turn a physics body: a force, a torque or a one-time impulse", "Type",
     merged_detail::options(merged_detail::force_options), {}},
    {"LogicNativeApplyTransform", "Apply Movement",
     "Move or turn an object by an amount, or move it by a speed", "Type",
     merged_detail::options(merged_detail::movement_options), {}},
    {"LogicNativeDriveVehicle", "Drive Vehicle", "Accelerate, brake or steer a vehicle's wheels",
     "Action", merged_detail::options(merged_detail::vehicle_options), {}},
    {"LogicNativeSetCharacterAttribute", "Set Character Attribute",
     "Set a character's gravity, jump force or number of jumps", "Attribute",
     merged_detail::options(merged_detail::character_options), {}},
    {"LogicNativeSetLightAttribute", "Set Light Attribute",
     "Set a light's color, power or shadow (every object using that light changes)",
     "Attribute", merged_detail::options(merged_detail::set_light_options), {}},
    {"LogicNativeGetLightAttribute", "Get Light Attribute", "Read a light's color or power",
     "Attribute", merged_detail::options(merged_detail::get_light_options), {}},
    {"LogicNativeSetCameraLens", "Set Camera Lens",
     "Set a camera's field of view or orthographic scale", "Attribute",
     merged_detail::options(merged_detail::camera_lens_options), {}},
    {"LogicNativeShowDebugOverlay", "Show Debug Overlay",
     "Show or hide the frame rate or profile overlay", "Show",
     merged_detail::options(merged_detail::debug_overlay_options), {}},
    {"LogicNativeControlSound", "Control Sound",
     "Pause, resume or stop a sound, or stop every sound", "Action",
     merged_detail::options(merged_detail::sound_options), {}},
    {"LogicNativeSetPaused", "Set Paused", "Pause the game or only its physics", "What",
     merged_detail::options(merged_detail::paused_options), {}},
    {"LogicNativeChangeScene", "Change Scene",
     "Replace this scene by another, or add another scene to this one", "Mode",
     merged_detail::options(merged_detail::scene_options), {}},
    {"LogicNativeOverlayCollection", "Overlay Collection",
     "Show a collection on top of a camera's view, or remove it", "Action",
     merged_detail::options(merged_detail::overlay_options), {}},
    {"LogicNativeGetChildObject", "Get Child", "Find a child of an object by its place or name",
     "By", merged_detail::options(merged_detail::child_options), {}},
    {"LogicNativeIsNone", "Is None", "Test whether a value is None", "Test",
     merged_detail::options(merged_detail::is_none_options), {}},
    {"LogicNativeVariableLoad", "Load Variable",
     "Read one saved variable, or all of a file's variables as a dictionary", "What",
     merged_detail::options(merged_detail::load_variable_options), {}},
    {"LogicNativeVariableSave", "Save Variable",
     "Save one variable, or a dictionary as the whole file", "What",
     merged_detail::options(merged_detail::save_variable_options), {}},
    {"LogicNativeVariableRemove", "Remove Variable",
     "Remove one saved variable, or all of a file's variables", "What",
     merged_detail::options(merged_detail::remove_variable_options), {}},
    {"LogicNativeGetBonePoint", "Get Bone Point",
     "A bone's head, tail or middle, posed or at rest, or its length", "Point",
     merged_detail::options(merged_detail::bone_point_options), {}},
    {"LogicNativeSetBoneConstraint", "Set Bone Constraint",
     "Set a bone constraint's influence, target or another setting", "Set",
     merged_detail::options(merged_detail::bone_constraint_options), {}},
    {"LogicNativeCursor", "Cursor", "Where the cursor is, or how far it moved", "Read",
     merged_detail::options(merged_detail::cursor_options), {}},
    {"LogicNativeGetDisplaySetting", "Get Display Setting",
     "Read the window's fullscreen state, resolution or vertical sync", "Setting",
     merged_detail::options(merged_detail::display_options), {}},
    {"LogicNativeThresholdValue", "Threshold",
     "Let a value through when it is above or below a threshold, or within or outside a range",
     "Pass When", merged_detail::options(merged_detail::threshold_options), {}},
    {"LogicNativeDelayFlow", "Delay",
     "Continue the flow after some seconds; choose what a new flow does while waiting",
     "New Flow While Waiting", merged_detail::options(merged_detail::delay_options), {}},
};

inline constexpr int merged_node_count = int(std::size(all_merged_nodes));

inline Span<MergedNode> merged_nodes()
{
  return Span<MergedNode>(all_merged_nodes, merged_node_count);
}

inline const MergedNode *find_merged_node(const StringRef idname)
{
  for (const MergedNode &merged : merged_nodes()) {
    if (idname == merged.idname) {
      return &merged;
    }
  }
  return nullptr;
}

/** The node's option (custom4 holds whole numbers; anything out of range is the first). */
inline int merged_option_index(const MergedNode &merged, const bNode &node)
{
  const int index = int(node.custom4 + 0.5f);
  return (index >= 0 && index < int(merged.options.size())) ? index : 0;
}

/** The older node the merged node is now. */
inline const char *merged_option_idname(const MergedNode &merged, const bNode &node)
{
  return merged.options[merged_option_index(merged, node)].idname;
}

/** An option's node type by its former idname (an older node set up for its merged node,
 * kept out of Blender's node registry: logic_option_register in node_logic_util.cc), or null.
 * Defined in the editor's logic nodes (bf_nodes_logic). */
const bke::bNodeType *find_option_node_type(StringRef idname);

/** The merged node one of whose options is the former node `idname`, or null. */
inline const MergedNode *find_merged_node_of_option(const StringRef idname,
                                                    int *r_option = nullptr)
{
  for (const MergedNode &merged : merged_nodes()) {
    for (const int option : merged.options.index_range()) {
      if (idname == merged.options[option].idname) {
        if (r_option != nullptr) {
          *r_option = option;
        }
        return &merged;
      }
    }
  }
  return nullptr;
}

}  // namespace blender::nodes::logic
