/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler_helpers_inputs.cc
 *  \ingroup logicnodes
 *
 * Compiler helpers, constant builders, and BuildInput* expression wiring.
 */

#include "LN_TreeCompiler_internal.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DNA_collection_types.h"
#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_sound_types.h"
#include "DNA_vfont_types.h"
#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"
#include "SCA_JoystickSensor.h"

#include "BL_Action.h"

namespace ln_compiler {

std::string SafeString(const char *value)
{
  return value ? std::string(value) : std::string();
}

bool LogicPinTypesCompatible(const LN_ValueType from_type, const LN_ValueType to_type)
{
  if (from_type == to_type) {
    return true;
  }
  if ((from_type == LN_ValueType::List && to_type == LN_ValueType::Vector) ||
      (from_type == LN_ValueType::Vector && to_type == LN_ValueType::List))
  {
    return true;
  }
  if (to_type == LN_ValueType::Generic) {
    return from_type != LN_ValueType::None;
  }
  if (from_type != LN_ValueType::Generic) {
    return false;
  }
  switch (to_type) {
    case LN_ValueType::Bool:
    case LN_ValueType::Int:
    case LN_ValueType::Float:
    case LN_ValueType::String:
    case LN_ValueType::Vector:
    case LN_ValueType::Vector4:
    case LN_ValueType::Matrix:
    case LN_ValueType::Color:
    case LN_ValueType::Rotation:
    case LN_ValueType::ObjectRef:
      return true;
    default:
      return false;
  }
}

bool LogicPinsCompatible(const LN_PinDefinition &from_pin, const LN_PinDefinition &to_pin)
{
  if (to_pin.kind == LN_PinKind::Execution) {
    return from_pin.kind == LN_PinKind::Execution;
  }
  if (from_pin.kind == LN_PinKind::Execution) {
    return false;
  }
  return LogicPinTypesCompatible(from_pin.value_type, to_pin.value_type);
}

std::string UpperIdentifier(std::string value)
{
  for (char &character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    character = static_cast<char>(std::toupper(byte));
  }
  return value;
}

std::string NormalizeInputEventName(std::string name)
{
  name = UpperIdentifier(std::move(name));
  const std::string prefix = "BGE.EVENTS.";
  if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
    name.erase(0, prefix.size());
  }
  return name;
}

static std::string CompactInputEventName(const std::string &name)
{
  std::string key = NormalizeInputEventName(name);
  key.erase(std::remove(key.begin(), key.end(), '_'), key.end());

  const std::string key_suffix = "KEY";
  if (key != "NOKEY" && key.size() > key_suffix.size() &&
      key.compare(key.size() - key_suffix.size(), key_suffix.size(), key_suffix) == 0)
  {
    key.resize(key.size() - key_suffix.size());
  }
  return key;
}

std::string MouseButtonNameFromCustom(const int custom2)
{
  switch (custom2) {
    case 1:
      return "MIDDLEMOUSE";
    case 2:
      return "RIGHTMOUSE";
    default:
      return "LEFTMOUSE";
  }
}

int32_t MouseInputStatusFromCustom(const int custom1)
{
  switch (custom1) {
    case 1:
      return SCA_InputEvent::ACTIVE;
    case 2:
      return SCA_InputEvent::JUSTRELEASED;
    default:
      return SCA_InputEvent::JUSTACTIVATED;
  }
}

LN_Value MakeNoneValue()
{
  LN_Value value;
  value.type = LN_ValueType::None;
  value.exists = false;
  return value;
}

LN_Value MakeDefaultValue(const LN_ValueType value_type)
{
  LN_Value value;
  value.type = value_type;
  value.exists = false;
  if (value_type == LN_ValueType::Color) {
    value.color_value = MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  else if (value_type == LN_ValueType::Matrix) {
    value.matrix_value = MT_Matrix3x3::Identity();
  }
  return value;
}

SCA_IInputDevice::SCA_EnumInputs InputCodeFromName(const std::string &name)
{
  const std::string key = CompactInputEventName(name);
  if (key.size() == 1) {
    const char character = key[0];
    if (character >= 'A' && character <= 'Z') {
      return SCA_IInputDevice::SCA_EnumInputs(SCA_IInputDevice::AKEY + (character - 'A'));
    }
    if (character >= '0' && character <= '9') {
      return SCA_IInputDevice::SCA_EnumInputs(SCA_IInputDevice::ZEROKEY + (character - '0'));
    }
  }

  if (key.size() > 1 && key[0] == 'F') {
    const int function_key = std::atoi(key.c_str() + 1);
    if (function_key >= 1 && function_key <= 19 &&
        key == "F" + std::to_string(function_key))
    {
      return SCA_IInputDevice::SCA_EnumInputs(SCA_IInputDevice::F1KEY + (function_key - 1));
    }
  }

  static const std::unordered_map<std::string, SCA_IInputDevice::SCA_EnumInputs> aliases = {
      {"ZERO", SCA_IInputDevice::ZEROKEY},
      {"ONE", SCA_IInputDevice::ONEKEY},
      {"TWO", SCA_IInputDevice::TWOKEY},
      {"THREE", SCA_IInputDevice::THREEKEY},
      {"FOUR", SCA_IInputDevice::FOURKEY},
      {"FIVE", SCA_IInputDevice::FIVEKEY},
      {"SIX", SCA_IInputDevice::SIXKEY},
      {"SEVEN", SCA_IInputDevice::SEVENKEY},
      {"EIGHT", SCA_IInputDevice::EIGHTKEY},
      {"NINE", SCA_IInputDevice::NINEKEY},

      {"RET", SCA_IInputDevice::RETKEY},
      {"ENTER", SCA_IInputDevice::RETKEY},
      {"RETURN", SCA_IInputDevice::RETKEY},
      {"SPACE", SCA_IInputDevice::SPACEKEY},
      {"ESC", SCA_IInputDevice::ESCKEY},
      {"ESCAPE", SCA_IInputDevice::ESCKEY},
      {"TAB", SCA_IInputDevice::TABKEY},
      {"LINEFEED", SCA_IInputDevice::LINEFEEDKEY},
      {"BACKSPACE", SCA_IInputDevice::BACKSPACEKEY},
      {"DEL", SCA_IInputDevice::DELKEY},
      {"DELETE", SCA_IInputDevice::DELKEY},
      {"SEMICOLON", SCA_IInputDevice::SEMICOLONKEY},
      {"PERIOD", SCA_IInputDevice::PERIODKEY},
      {"COMMA", SCA_IInputDevice::COMMAKEY},
      {"QUOTE", SCA_IInputDevice::QUOTEKEY},
      {"ACCENTGRAVE", SCA_IInputDevice::ACCENTGRAVEKEY},
      {"MINUS", SCA_IInputDevice::MINUSKEY},
      {"SLASH", SCA_IInputDevice::SLASHKEY},
      {"BACKSLASH", SCA_IInputDevice::BACKSLASHKEY},
      {"EQUAL", SCA_IInputDevice::EQUALKEY},
      {"LEFTBRACKET", SCA_IInputDevice::LEFTBRACKETKEY},
      {"RIGHTBRACKET", SCA_IInputDevice::RIGHTBRACKETKEY},

      {"LEFT", SCA_IInputDevice::LEFTARROWKEY},
      {"LEFTARROW", SCA_IInputDevice::LEFTARROWKEY},
      {"RIGHT", SCA_IInputDevice::RIGHTARROWKEY},
      {"RIGHTARROW", SCA_IInputDevice::RIGHTARROWKEY},
      {"UP", SCA_IInputDevice::UPARROWKEY},
      {"UPARROW", SCA_IInputDevice::UPARROWKEY},
      {"DOWN", SCA_IInputDevice::DOWNARROWKEY},
      {"DOWNARROW", SCA_IInputDevice::DOWNARROWKEY},

      {"LEFTCTRL", SCA_IInputDevice::LEFTCTRLKEY},
      {"LEFTCONTROL", SCA_IInputDevice::LEFTCTRLKEY},
      {"LEFTALT", SCA_IInputDevice::LEFTALTKEY},
      {"RIGHTALT", SCA_IInputDevice::RIGHTALTKEY},
      {"RIGHTCTRL", SCA_IInputDevice::RIGHTCTRLKEY},
      {"RIGHTCONTROL", SCA_IInputDevice::RIGHTCTRLKEY},
      {"RIGHTSHIFT", SCA_IInputDevice::RIGHTSHIFTKEY},
      {"LEFTSHIFT", SCA_IInputDevice::LEFTSHIFTKEY},
      {"OS", SCA_IInputDevice::OSKEY},
      {"OSKEY", SCA_IInputDevice::OSKEY},
      {"PAUSE", SCA_IInputDevice::PAUSEKEY},
      {"INSERT", SCA_IInputDevice::INSERTKEY},
      {"HOME", SCA_IInputDevice::HOMEKEY},
      {"PAGEUP", SCA_IInputDevice::PAGEUPKEY},
      {"PAGEDOWN", SCA_IInputDevice::PAGEDOWNKEY},
      {"END", SCA_IInputDevice::ENDKEY},

      {"NUMPAD0", SCA_IInputDevice::PAD0},
      {"NUMPAD1", SCA_IInputDevice::PAD1},
      {"NUMPAD2", SCA_IInputDevice::PAD2},
      {"NUMPAD3", SCA_IInputDevice::PAD3},
      {"NUMPAD4", SCA_IInputDevice::PAD4},
      {"NUMPAD5", SCA_IInputDevice::PAD5},
      {"NUMPAD6", SCA_IInputDevice::PAD6},
      {"NUMPAD7", SCA_IInputDevice::PAD7},
      {"NUMPAD8", SCA_IInputDevice::PAD8},
      {"NUMPAD9", SCA_IInputDevice::PAD9},
      {"PAD0", SCA_IInputDevice::PAD0},
      {"PAD1", SCA_IInputDevice::PAD1},
      {"PAD2", SCA_IInputDevice::PAD2},
      {"PAD3", SCA_IInputDevice::PAD3},
      {"PAD4", SCA_IInputDevice::PAD4},
      {"PAD5", SCA_IInputDevice::PAD5},
      {"PAD6", SCA_IInputDevice::PAD6},
      {"PAD7", SCA_IInputDevice::PAD7},
      {"PAD8", SCA_IInputDevice::PAD8},
      {"PAD9", SCA_IInputDevice::PAD9},
      {"NUMPADPERIOD", SCA_IInputDevice::PADPERIOD},
      {"PADPERIOD", SCA_IInputDevice::PADPERIOD},
      {"NUMPADSLASH", SCA_IInputDevice::PADSLASHKEY},
      {"PADSLASH", SCA_IInputDevice::PADSLASHKEY},
      {"NUMPADASTER", SCA_IInputDevice::PADASTERKEY},
      {"NUMPADASTERIX", SCA_IInputDevice::PADASTERKEY},
      {"PADASTER", SCA_IInputDevice::PADASTERKEY},
      {"PADASTERIX", SCA_IInputDevice::PADASTERKEY},
      {"NUMPADMINUS", SCA_IInputDevice::PADMINUS},
      {"PADMINUS", SCA_IInputDevice::PADMINUS},
      {"NUMPADENTER", SCA_IInputDevice::PADENTER},
      {"PADENTER", SCA_IInputDevice::PADENTER},
      {"NUMPADPLUS", SCA_IInputDevice::PADPLUSKEY},
      {"PADPLUS", SCA_IInputDevice::PADPLUSKEY},

      {"LEFTMOUSE", SCA_IInputDevice::LEFTMOUSE},
      {"MIDDLEMOUSE", SCA_IInputDevice::MIDDLEMOUSE},
      {"RIGHTMOUSE", SCA_IInputDevice::RIGHTMOUSE},
      {"BUTTON4MOUSE", SCA_IInputDevice::BUTTON4MOUSE},
      {"BUTTON5MOUSE", SCA_IInputDevice::BUTTON5MOUSE},
      {"BUTTON6MOUSE", SCA_IInputDevice::BUTTON6MOUSE},
      {"BUTTON7MOUSE", SCA_IInputDevice::BUTTON7MOUSE},
      {"WHEELUP", SCA_IInputDevice::WHEELUPMOUSE},
      {"WHEELUPMOUSE", SCA_IInputDevice::WHEELUPMOUSE},
      {"WHEELDOWN", SCA_IInputDevice::WHEELDOWNMOUSE},
      {"WHEELDOWNMOUSE", SCA_IInputDevice::WHEELDOWNMOUSE},
      {"MOUSEX", SCA_IInputDevice::MOUSEX},
      {"MOUSEY", SCA_IInputDevice::MOUSEY},
  };

  const auto it = aliases.find(key);
  if (it != aliases.end()) {
    return it->second;
  }

  return SCA_IInputDevice::NOKEY;
}

int32_t GamepadButtonFromName(const std::string &name)
{
  const std::string key = UpperIdentifier(name);
  if (key == "A" || key == "CROSS") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_A;
  }
  if (key == "B" || key == "CIRCLE") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_B;
  }
  if (key == "X" || key == "SQUARE") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_X;
  }
  if (key == "Y" || key == "TRIANGLE") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_Y;
  }
  if (key == "START") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_START;
  }
  if (key == "BACK" || key == "SELECT") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_BACK;
  }
  if (key == "LEFTSHOULDER" || key == "LB") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_SHOULDER_LEFT;
  }
  if (key == "RIGHTSHOULDER" || key == "RB") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_SHOULDER_RIGHT;
  }
  if (key == "DPADUP" || key == "UP") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_UP;
  }
  if (key == "DPADDOWN" || key == "DOWN") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_DOWN;
  }
  if (key == "DPADLEFT" || key == "LEFT") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_LEFT;
  }
  if (key == "DPADRIGHT" || key == "RIGHT") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_RIGHT;
  }
  if (key == "LEFTSTICK" || key == "L3") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_STICK_LEFT;
  }
  if (key == "RIGHTSTICK" || key == "R3") {
    return SCA_JoystickSensor::KX_JOYSENS_BUTTON_STICK_RIGHT;
  }
  return SCA_JoystickSensor::KX_JOYSENS_BUTTON_A;
}

uint32_t GamepadStickFromName(const std::string &name)
{
  const std::string key = UpperIdentifier(name);
  return (key == "RIGHT" || key == "RIGHTSTICK") ? 1u : 0u;
}

bool GamepadAddonIndexIsTrigger(const int addon_index)
{
  return addon_index == 15 || addon_index == 16;
}

int32_t GamepadTriggerAxisFromAddonIndex(const int addon_index)
{
  /* Matches uplogic: axis indices 15/16 map to physical axes 4/5. */
  if (addon_index == 15) {
    return 4;
  }
  if (addon_index == 16) {
    return 5;
  }
  return 4;
}

int32_t GamepadButtonFromAddonIndex(const int addon_index)
{
  switch (addon_index) {
    case 0:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_A;
    case 1:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_B;
    case 2:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_X;
    case 3:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_Y;
    case 4:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_BACK;
    case 6:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_START;
    case 7:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_STICK_LEFT;
    case 8:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_STICK_RIGHT;
    case 9:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_SHOULDER_LEFT;
    case 10:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_SHOULDER_RIGHT;
    case 11:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_UP;
    case 12:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_DOWN;
    case 13:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_LEFT;
    case 14:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_DPAD_RIGHT;
    default:
      return SCA_JoystickSensor::KX_JOYSENS_BUTTON_A;
  }
}

GamepadButtonTarget GamepadButtonTargetFromNode(const blender::bNode &node,
                                                const NodeDefinitionMap &node_definitions,
                                                const InputLinkMap &input_links,
                                                ValueCache &value_cache)
{
  GamepadButtonTarget target{};
  const int32_t addon_index = node.custom2;

  target.is_trigger = GamepadAddonIndexIsTrigger(addon_index);
  target.index = target.is_trigger ? GamepadTriggerAxisFromAddonIndex(addon_index) :
                                     GamepadButtonFromAddonIndex(addon_index);
  return target;
}

std::string IDNameWithoutPrefix(const blender::ID &id)
{
  if (id.name[0] != '\0' && id.name[1] != '\0') {
    return id.name + 2;
  }
  return id.name;
}

std::string NodeDisplayName(const blender::bNode &node)
{
  if (node.label[0] != '\0') {
    return node.label;
  }
  if (node.name[0] != '\0') {
    return node.name;
  }
  return node.idname;
}

std::string AddObjectResultPropertyName(const blender::bNode &node)
{
  return "__native_add_object_result_" + std::to_string(node.identifier);
}

std::string AssignGeometryNodesModifierResultPropertyName(const blender::bNode &node)
{
  return "__native_assign_geometry_modifier_result_" + std::to_string(node.identifier);
}

bool NamesMatch(const char *socket_name, const char *identifier, const std::string &name)
{
  if (name == "Out" &&
      (std::strcmp(socket_name, "Pulse") == 0 || std::strcmp(identifier, "Pulse") == 0))
  {
    return true;
  }
  if (name == "Message" &&
      (std::strcmp(socket_name, "Value") == 0 || std::strcmp(identifier, "Value") == 0))
  {
    return true;
  }
  return std::strcmp(socket_name, name.c_str()) == 0 ||
         std::strcmp(identifier, name.c_str()) == 0;
}

static bool SocketIdnamesCompatible(const char *actual, const std::string &expected)
{
  if (expected.empty() || actual[0] == '\0') {
    return true;
  }
  if (std::strcmp(actual, expected.c_str()) == 0) {
    return true;
  }
  const bool actual_is_int = std::strcmp(actual, "NodeSocketLogicInt") == 0;
  const bool actual_is_collision_layers =
      std::strcmp(actual, "NodeSocketLogicCollisionLayers") == 0;
  const bool actual_is_old_collision_layers_socket =
      std::strcmp(actual, "NodeSocketLogicCollisionCollections") == 0;
  return (expected == "NodeSocketLogicInt" &&
          (actual_is_collision_layers || actual_is_old_collision_layers_socket)) ||
         (expected == "NodeSocketLogicCollisionLayers" &&
          (actual_is_int || actual_is_old_collision_layers_socket));
}

static bool SocketMatchesPinDefinition(const LN_PinDefinition &pin,
                                       const blender::bNodeSocket &socket)
{
  if (!NamesMatch(socket.name, socket.identifier, pin.name)) {
    return false;
  }
  return SocketIdnamesCompatible(socket.idname, pin.socket_idname);
}

const blender::bNodeSocket *FindInputSocket(const blender::bNode &node,
                                            const std::string &name)
{
  return FindInputSocketExact(node, name);
}

const blender::bNodeSocket *FindInputSocketExact(const blender::bNode &node,
                                                 const std::string &name)
{
  for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
           node.inputs.first);
       socket;
       socket = socket->next)
  {
    if (NamesMatch(socket->name, socket->identifier, name)) {
      return socket;
    }
  }
  return nullptr;
}

bool IsInputSocketLinked(const blender::bNode &node,
                         const std::string &socket_name,
                         const InputLinkMap &input_links)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return false;
  }
  const auto links_iter = input_links.find(socket);
  return links_iter != input_links.end() && !links_iter->second.empty();
}

const blender::bNodeSocket *FindOutputSocket(const blender::bNode &node,
                                             const std::string &name)
{
  for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
           node.outputs.first);
       socket;
       socket = socket->next)
  {
    if (NamesMatch(socket->name, socket->identifier, name)) {
      return socket;
    }
  }
  return nullptr;
}

const LN_PinDefinition *FindPinDefinition(const std::vector<LN_PinDefinition> &pins,
                                          const blender::bNodeSocket &socket)
{
  for (const LN_PinDefinition &pin : pins) {
    if (SocketMatchesPinDefinition(pin, socket)) {
      return &pin;
    }
  }
  return nullptr;
}

const LN_PinDefinition *FindFirstExecutionInputPin(const LN_NodeDefinition &definition)
{
  for (const LN_PinDefinition &pin : definition.inputs) {
    if (pin.kind == LN_PinKind::Execution) {
      return &pin;
    }
  }
  return nullptr;
}

const LN_PinDefinition *FindFirstExecutionOutputPin(const LN_NodeDefinition &definition)
{
  for (const LN_PinDefinition &pin : definition.outputs) {
    if (pin.kind == LN_PinKind::Execution) {
      return &pin;
    }
  }
  return nullptr;
}

bool IsExecutionOutputSocket(const LN_NodeDefinition &definition,
                             const blender::bNodeSocket &socket)
{
  const LN_PinDefinition *pin = FindPinDefinition(definition.outputs, socket);
  return pin != nullptr && pin->kind == LN_PinKind::Execution;
}

std::optional<uint32_t> BuildPrimaryExecutionExpression(
    LN_Program &program,
    const blender::bNode &node,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const LN_PinDefinition *flow_pin = FindFirstExecutionInputPin(definition);
  if (flow_pin == nullptr) {
    return std::nullopt;
  }

  return BuildInputExecutionExpression(program,
                                       node,
                                       flow_pin->name,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       bool_expression_cache);
}

bool IsUsedLink(const blender::bNodeLink &link)
{
  return link.fromnode != nullptr && link.tonode != nullptr && link.fromsock != nullptr &&
         link.tosock != nullptr && (int(link.flag) & int(blender::NODE_LINK_MUTED)) == 0;
}

bool IsFrameNode(const blender::bNode &node)
{
  return std::strcmp(node.idname, "NodeFrame") == 0;
}

bool IsRerouteNode(const blender::bNode &node)
{
  return std::strcmp(node.idname, "NodeReroute") == 0;
}

const blender::bNodeSocket *FirstInputSocket(const blender::bNode &node)
{
  return static_cast<const blender::bNodeSocket *>(node.inputs.first);
}

const blender::bNodeSocket *FirstOutputSocket(const blender::bNode &node)
{
  return static_cast<const blender::bNodeSocket *>(node.outputs.first);
}

std::optional<ResolvedEndpoint> ResolveSourceEndpoint(
    const blender::bNode &node,
    const blender::bNodeSocket &socket,
    const RawInputLinkMap &raw_input_links,
    std::unordered_set<const blender::bNode *> &visited_reroutes)
{
  if (!IsRerouteNode(node)) {
    return ResolvedEndpoint{&node, &socket};
  }

  if (!visited_reroutes.insert(&node).second) {
    return std::nullopt;
  }

  const blender::bNodeSocket *input_socket = FirstInputSocket(node);
  if (input_socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = raw_input_links.find(input_socket);
  if (links_iter == raw_input_links.end() || links_iter->second.empty()) {
    return std::nullopt;
  }

  const blender::bNodeLink *link = links_iter->second.front();
  return ResolveSourceEndpoint(*link->fromnode, *link->fromsock, raw_input_links, visited_reroutes);
}

void AppendTargetEndpoints(const blender::bNode &node,
                          const blender::bNodeSocket &socket,
                          const RawOutputLinkMap &raw_output_links,
                          std::vector<ResolvedEndpoint> &r_targets,
                          std::unordered_set<const blender::bNode *> &visited_reroutes)
{
  if (!IsRerouteNode(node)) {
    r_targets.push_back({&node, &socket});
    return;
  }

  if (!visited_reroutes.insert(&node).second) {
    return;
  }

  const blender::bNodeSocket *output_socket = FirstOutputSocket(node);
  if (output_socket == nullptr) {
    return;
  }

  const auto links_iter = raw_output_links.find(output_socket);
  if (links_iter == raw_output_links.end()) {
    return;
  }

  for (const blender::bNodeLink *link : links_iter->second) {
    AppendTargetEndpoints(*link->tonode,
                          *link->tosock,
                          raw_output_links,
                          r_targets,
                          visited_reroutes);
  }
}

void HashBytes(uint64_t &hash, const void *data, size_t size)
{
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
}

void HashString(uint64_t &hash, const char *value)
{
  const char *safe_value = value ? value : "";
  HashBytes(hash, safe_value, std::strlen(safe_value));
  const char separator = '\0';
  HashBytes(hash, &separator, sizeof(separator));
}

template<typename T> void HashValue(uint64_t &hash, const T &value)
{
  HashBytes(hash, &value, sizeof(value));
}

void HashSocketDefault(uint64_t &hash, const blender::bNodeSocket &socket)
{
  HashString(hash, socket.identifier);
  HashString(hash, socket.name);
  HashString(hash, socket.idname);
  HashValue(hash, socket.type);

  if (socket.default_value == nullptr) {
    return;
  }

  switch (socket.type) {
    case blender::SOCK_BOOLEAN: {
      const blender::bNodeSocketValueBoolean &value =
          *static_cast<const blender::bNodeSocketValueBoolean *>(socket.default_value);
      HashValue(hash, value.value);
      break;
    }
    case blender::SOCK_FLOAT: {
      const blender::bNodeSocketValueFloat &value =
          *static_cast<const blender::bNodeSocketValueFloat *>(socket.default_value);
      HashValue(hash, value.value);
      break;
    }
    case blender::SOCK_INT: {
      const blender::bNodeSocketValueInt &value =
          *static_cast<const blender::bNodeSocketValueInt *>(socket.default_value);
      HashValue(hash, value.value);
      break;
    }
    case blender::SOCK_VECTOR: {
      const blender::bNodeSocketValueVector &value =
          *static_cast<const blender::bNodeSocketValueVector *>(socket.default_value);
      HashBytes(hash, value.value, sizeof(value.value));
      HashValue(hash, value.dimensions);
      break;
    }
    case blender::SOCK_RGBA: {
      const blender::bNodeSocketValueRGBA &value =
          *static_cast<const blender::bNodeSocketValueRGBA *>(socket.default_value);
      HashBytes(hash, value.value, sizeof(value.value));
      break;
    }
    case blender::SOCK_STRING: {
      const blender::bNodeSocketValueString &value =
          *static_cast<const blender::bNodeSocketValueString *>(socket.default_value);
      HashString(hash, value.value);
      break;
    }
    case blender::SOCK_MATERIAL: {
      const blender::bNodeSocketValueMaterial &value =
          *static_cast<const blender::bNodeSocketValueMaterial *>(socket.default_value);
      HashString(hash, value.value ? value.value->id.name : "");
      break;
    }
    default:
      break;
  }
}

std::string BuildChecksum(const blender::bNodeTree &tree)
{
  uint64_t hash = FNV_OFFSET_BASIS;
  HashString(hash, tree.idname);
  HashValue(hash, tree.type);

  for (const blender::bNode *node = static_cast<const blender::bNode *>(tree.nodes.first);
       node;
       node = node->next)
  {
    HashString(hash, node->idname);
    HashString(hash, node->name);
    HashValue(hash, node->identifier);
    HashValue(hash, node->custom1);
    HashValue(hash, node->custom2);

    for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
             node->inputs.first);
         socket;
         socket = socket->next)
    {
      HashSocketDefault(hash, *socket);
    }
    for (const blender::bNodeSocket *socket = static_cast<const blender::bNodeSocket *>(
             node->outputs.first);
         socket;
         socket = socket->next)
    {
      HashSocketDefault(hash, *socket);
    }
  }

  for (const blender::bNodeLink *link = static_cast<const blender::bNodeLink *>(
           tree.links.first);
       link;
       link = link->next)
  {
    if (!IsUsedLink(*link)) {
      continue;
    }
    HashValue(hash, link->fromnode->identifier);
    HashString(hash, link->fromsock->identifier);
    HashString(hash, link->fromsock->name);
    HashValue(hash, link->tonode->identifier);
    HashString(hash, link->tosock->identifier);
    HashString(hash, link->tosock->name);
  }

  std::ostringstream stream;
  stream << "lnv1-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return stream.str();
}

uint32_t AddSourceRef(LN_Program &program,
                      const blender::bNodeTree &tree,
                      const blender::bNode &node,
                      const char *socket_name)
{
  LN_SourceRef source_ref;
  source_ref.source_node_identifier = node.identifier;
  source_ref.node_idname = SafeString(node.idname);
  source_ref.node_name = NodeDisplayName(node);
  source_ref.socket_name = SafeString(socket_name);
  source_ref.source_tree_name = IDNameWithoutPrefix(tree.id);
  source_ref.source_tree_library_path = tree.id.lib ? SafeString(tree.id.lib->filepath) : "";
  return program.AddSourceRef(source_ref);
}

void AddNodeIssue(LN_Program &program,
                  const blender::bNodeTree &tree,
                  const blender::bNode &node,
                  const char *socket_name,
                  LN_CompileSeverity severity,
                  const std::string &message)
{
  const uint32_t source_ref_index = AddSourceRef(program, tree, node, socket_name);
  program.AddCompileIssue(severity, message, source_ref_index);
}

std::optional<LN_Value> ReadSocketDefault(const blender::bNodeSocket &socket,
                                          LN_ValueType expected_type)
{
  if (socket.default_value == nullptr) {
    return std::nullopt;
  }

  LN_Value value;
  value.type = expected_type;
  switch (expected_type) {
    case LN_ValueType::Bool: {
      const blender::bNodeSocketValueBoolean &socket_value =
          *static_cast<const blender::bNodeSocketValueBoolean *>(socket.default_value);
      value.bool_value = socket_value.value != 0;
      return value;
    }
    case LN_ValueType::Int: {
      const blender::bNodeSocketValueInt &socket_value =
          *static_cast<const blender::bNodeSocketValueInt *>(socket.default_value);
      value.int_value = socket_value.value;
      return value;
    }
    case LN_ValueType::Float: {
      const blender::bNodeSocketValueFloat &socket_value =
          *static_cast<const blender::bNodeSocketValueFloat *>(socket.default_value);
      value.float_value = socket_value.value;
      return value;
    }
    case LN_ValueType::Vector: {
      const blender::bNodeSocketValueVector &socket_value =
          *static_cast<const blender::bNodeSocketValueVector *>(socket.default_value);
      value.vector_value = MT_Vector3(socket_value.value[0],
                                      socket_value.value[1],
                                      socket_value.value[2]);
      return value;
    }
    case LN_ValueType::Vector4:
    case LN_ValueType::Matrix:
      return std::nullopt;
    case LN_ValueType::Color: {
      const blender::bNodeSocketValueRGBA &socket_value =
          *static_cast<const blender::bNodeSocketValueRGBA *>(socket.default_value);
      value.color_value = MT_Vector4(socket_value.value[0],
                                     socket_value.value[1],
                                     socket_value.value[2],
                                     socket_value.value[3]);
      return value;
    }
    case LN_ValueType::Rotation: {
      const blender::bNodeSocketValueRotation &socket_value =
          *static_cast<const blender::bNodeSocketValueRotation *>(socket.default_value);
      value.rotation_euler_value = MT_Vector3(socket_value.value_euler[0],
                                             socket_value.value_euler[1],
                                             socket_value.value_euler[2]);
      return value;
    }
    case LN_ValueType::String: {
      const blender::bNodeSocketValueString &socket_value =
          *static_cast<const blender::bNodeSocketValueString *>(socket.default_value);
      value.string_value = socket_value.value;
      return value;
    }
    case LN_ValueType::ObjectRef: {
      const blender::bNodeSocketValueObject &socket_value =
          *static_cast<const blender::bNodeSocketValueObject *>(socket.default_value);
      value.exists = socket_value.value != nullptr;
      if (socket_value.value != nullptr) {
        value.reference_name = socket_value.value->id.name + 2;
      }
      return value;
    }
    case LN_ValueType::CollectionRef: {
      if (socket.type != blender::SOCK_COLLECTION) {
        return std::nullopt;
      }
      const blender::bNodeSocketValueCollection &socket_value =
          *static_cast<const blender::bNodeSocketValueCollection *>(socket.default_value);
      value.exists = socket_value.value != nullptr;
      if (socket_value.value != nullptr) {
        value.reference_name = socket_value.value->id.name + 2;
      }
      return value;
    }
    case LN_ValueType::DatablockRef: {
      blender::ID *id = nullptr;
      if (socket.type == blender::SOCK_MATERIAL) {
        const blender::bNodeSocketValueMaterial &socket_value =
            *static_cast<const blender::bNodeSocketValueMaterial *>(socket.default_value);
        id = socket_value.value ? &socket_value.value->id : nullptr;
      }
      else if (socket.type == blender::SOCK_IMAGE) {
        const blender::bNodeSocketValueImage &socket_value =
            *static_cast<const blender::bNodeSocketValueImage *>(socket.default_value);
        id = socket_value.value ? &socket_value.value->id : nullptr;
      }
      else if (socket.type == blender::SOCK_SOUND) {
        const blender::bNodeSocketValueSound &socket_value =
            *static_cast<const blender::bNodeSocketValueSound *>(socket.default_value);
        id = socket_value.value ? &socket_value.value->id : nullptr;
      }
      else if (socket.type == blender::SOCK_FONT) {
        const blender::bNodeSocketValueFont &socket_value =
            *static_cast<const blender::bNodeSocketValueFont *>(socket.default_value);
        id = socket_value.value ? &socket_value.value->id : nullptr;
      }
      else {
        return std::nullopt;
      }
      value.exists = id != nullptr;
      if (id != nullptr) {
        value.reference_name = id->name + 2;
      }
      return value;
    }
    case LN_ValueType::SceneRef:
    case LN_ValueType::List:
    case LN_ValueType::Dict:
    case LN_ValueType::Generic:
    case LN_ValueType::None:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<LN_Value> ReadInputValue(
    const blender::bNode &node,
    const std::string &socket_name,
    LN_ValueType expected_type,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter != node_definitions.end()) {
    if (const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->inputs, *socket)) {
      if (pin->kind == LN_PinKind::Execution) {
        return std::nullopt;
      }
    }
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    return EvaluateOutputValue(*link.fromnode,
                               *link.fromsock,
                               node_definitions,
                               input_links,
                               value_cache);
  }

  return ReadSocketDefault(*socket, expected_type);
}

std::optional<float> EvaluateMath(int16_t operation, float a, float b)
{
  switch (operation) {
    case blender::NODE_MATH_ADD:
      return a + b;
    case blender::NODE_MATH_SUBTRACT:
      return a - b;
    case blender::NODE_MATH_MULTIPLY:
      return a * b;
    case blender::NODE_MATH_DIVIDE:
      if (std::fabs(b) <= 1.0e-20f) {
        return 0.0f;
      }
      return a / b;
    case blender::NODE_MATH_POWER:
      return std::pow(a, b);
    case blender::NODE_MATH_MINIMUM:
      return std::min(a, b);
    case blender::NODE_MATH_MAXIMUM:
      return std::max(a, b);
    case blender::NODE_MATH_ABSOLUTE:
      return std::fabs(a);
    case blender::NODE_MATH_SIGN:
      return float((a > 0.0f) - (a < 0.0f));
    case blender::NODE_MATH_ROUND:
      return std::floor(a + 0.5f);
    case blender::NODE_MATH_FLOOR:
      return std::floor(a);
    case blender::NODE_MATH_CEIL:
      return std::ceil(a);
    case blender::NODE_MATH_TRUNC:
      return std::trunc(a);
    case blender::NODE_MATH_FRACTION:
      return a - std::floor(a);
    case blender::NODE_MATH_MODULO:
      if (std::fabs(b) <= 1.0e-20f) {
        return 0.0f;
      }
      return std::fmod(a, b);
    case blender::NODE_MATH_SINE:
      return std::sin(a);
    case blender::NODE_MATH_COSINE:
      return std::cos(a);
    case blender::NODE_MATH_RADIANS:
      return a * (LN_PI / 180.0f);
    case blender::NODE_MATH_DEGREES:
      return a * (180.0f / LN_PI);
    default:
      return std::nullopt;
  }

  return std::nullopt;
}

bool IsSupportedLogicMathOperation(int16_t operation)
{
  switch (operation) {
    case blender::NODE_MATH_ADD:
    case blender::NODE_MATH_SUBTRACT:
    case blender::NODE_MATH_MULTIPLY:
    case blender::NODE_MATH_DIVIDE:
    case blender::NODE_MATH_POWER:
    case blender::NODE_MATH_MINIMUM:
    case blender::NODE_MATH_MAXIMUM:
    case blender::NODE_MATH_ABSOLUTE:
    case blender::NODE_MATH_SIGN:
    case blender::NODE_MATH_ROUND:
    case blender::NODE_MATH_FLOOR:
    case blender::NODE_MATH_CEIL:
    case blender::NODE_MATH_TRUNC:
    case blender::NODE_MATH_FRACTION:
    case blender::NODE_MATH_MODULO:
    case blender::NODE_MATH_SINE:
    case blender::NODE_MATH_COSINE:
    case blender::NODE_MATH_RADIANS:
    case blender::NODE_MATH_DEGREES:
      return true;
  }
  return false;
}

bool IsSupportedLogicVectorMathOperation(int16_t operation)
{
  switch (operation) {
    case blender::NODE_VECTOR_MATH_ADD:
    case blender::NODE_VECTOR_MATH_SUBTRACT:
    case blender::NODE_VECTOR_MATH_MULTIPLY:
    case blender::NODE_VECTOR_MATH_DIVIDE:
    case blender::NODE_VECTOR_MATH_ABSOLUTE:
    case blender::NODE_VECTOR_MATH_MINIMUM:
    case blender::NODE_VECTOR_MATH_MAXIMUM:
    case blender::NODE_VECTOR_MATH_SCALE:
    case blender::NODE_VECTOR_MATH_NORMALIZE:
      return true;
  }
  return false;
}

bool IsSupportedLogicStringOperation(int16_t operation)
{
  switch (operation) {
    case LN_STRING_OP_JOIN:
    case LN_STRING_OP_CONTAINS:
    case LN_STRING_OP_COUNT:
    case LN_STRING_OP_REPLACE:
    case LN_STRING_OP_STARTS_WITH:
    case LN_STRING_OP_ENDS_WITH:
    case LN_STRING_OP_UPPER:
    case LN_STRING_OP_LOWER:
    case LN_STRING_OP_ZFILL:
      return true;
  }
  return false;
}

bool StringStartsWith(const std::string &value, const std::string &prefix)
{
  return prefix.size() <= value.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool StringEndsWith(const std::string &value, const std::string &suffix)
{
  return suffix.size() <= value.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int32_t CountStringOccurrences(const std::string &value, const std::string &needle)
{
  if (needle.empty()) {
    return 0;
  }

  int32_t count = 0;
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    count++;
    pos += needle.size();
  }
  return count;
}

std::string ReplaceStringOccurrences(const std::string &value,
                                     const std::string &needle,
                                     const std::string &replacement)
{
  if (needle.empty()) {
    return value;
  }

  std::string result = value;
  size_t pos = 0;
  while ((pos = result.find(needle, pos)) != std::string::npos) {
    result.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return result;
}

std::string ToCaseString(const std::string &value, const bool uppercase)
{
  std::string result = value;
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    character = static_cast<char>(uppercase ? std::toupper(byte) : std::tolower(byte));
  }
  return result;
}

std::string ZeroFillString(const std::string &value, const int32_t width)
{
  if (width <= int32_t(value.size())) {
    return value;
  }

  const size_t sign_offset = (!value.empty() && (value[0] == '-' || value[0] == '+')) ? 1 : 0;
  std::string result = value;
  result.insert(sign_offset, size_t(width - int32_t(value.size())), '0');
  return result;
}

const std::string &FormatInputString(const size_t index,
                                     const std::string &a,
                                     const std::string &b,
                                     const std::string &c,
                                     const std::string &d)
{
  switch (index) {
    case 0:
      return a;
    case 1:
      return b;
    case 2:
      return c;
    default:
      return d;
  }
}

std::string FormatStringSlots(const std::string &format,
                              const std::string &a,
                              const std::string &b,
                              const std::string &c,
                              const std::string &d)
{
  std::string result;
  result.reserve(format.size() + a.size() + b.size() + c.size() + d.size());

  size_t input_index = 0;
  for (size_t index = 0; index < format.size(); index++) {
    if (index + 1 >= format.size()) {
      result.push_back(format[index]);
      continue;
    }

    const char current = format[index];
    const char next = format[index + 1];
    if (current == '{' && next == '{') {
      result.push_back('{');
      index++;
    }
    else if (current == '}' && next == '}') {
      result.push_back('}');
      index++;
    }
    else if (current == '{' && next == '}' && input_index < 4) {
      result += FormatInputString(input_index, a, b, c, d);
      input_index++;
      index++;
    }
    else {
      result.push_back(current);
    }
  }

  return result;
}

std::optional<LN_FloatCompareOperation> FloatCompareOperationFromCustom1(int16_t operation)
{
  switch (operation) {
    case 0:
      return LN_FloatCompareOperation::Equal;
    case 1:
      return LN_FloatCompareOperation::NotEqual;
    case 2:
      return LN_FloatCompareOperation::GreaterThan;
    case 3:
      return LN_FloatCompareOperation::LessThan;
    case 4:
      return LN_FloatCompareOperation::GreaterEqual;
    case 5:
      return LN_FloatCompareOperation::LessEqual;
    default:
      return std::nullopt;
  }
}

bool EvaluateFloatCompare(LN_FloatCompareOperation operation, float a, float b)
{
  switch (operation) {
    case LN_FloatCompareOperation::Equal:
      return std::fabs(a - b) <= 1.0e-6f;
    case LN_FloatCompareOperation::NotEqual:
      return std::fabs(a - b) > 1.0e-6f;
    case LN_FloatCompareOperation::GreaterThan:
      return a > b;
    case LN_FloatCompareOperation::LessThan:
      return a < b;
    case LN_FloatCompareOperation::GreaterEqual:
      return a >= b;
    case LN_FloatCompareOperation::LessEqual:
      return a <= b;
  }

  return false;
}

std::optional<LN_ThresholdOperation> ThresholdOperationFromCustom1(int16_t operation)
{
  switch (operation) {
    case 0:
      return LN_ThresholdOperation::Greater;
    case 1:
      return LN_ThresholdOperation::Less;
    default:
      return std::nullopt;
  }
}

std::optional<LN_RangeOperation> RangeOperationFromCustom1(int16_t operation)
{
  switch (operation) {
    case 0:
      return LN_RangeOperation::Inside;
    case 1:
      return LN_RangeOperation::Outside;
    default:
      return std::nullopt;
  }
}

float EvaluateThreshold(LN_ThresholdOperation operation,
                        float value,
                        float threshold,
                        bool else_zero)
{
  switch (operation) {
    case LN_ThresholdOperation::Greater:
      return (value > threshold) ? value : (else_zero ? 0.0f : threshold);
    case LN_ThresholdOperation::Less:
      return (value < threshold) ? value : (else_zero ? 0.0f : threshold);
  }
  return 0.0f;
}

bool EvaluateRange(LN_RangeOperation operation, float value, float min_value, float max_value)
{
  switch (operation) {
    case LN_RangeOperation::Inside:
      return min_value < value && value < max_value;
    case LN_RangeOperation::Outside:
      return value < min_value || value > max_value;
  }
  return false;
}

uint32_t AddConstantBoolExpression(LN_Program &program, bool value)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Constant;
  expression.bool_value = value;
  return program.AddBoolExpression(expression);
}

uint32_t AddConstantFloatExpression(LN_Program &program, float value)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::Constant;
  expression.float_value = value;
  return program.AddFloatExpression(expression);
}

uint32_t AddConstantIntExpression(LN_Program &program, int32_t value)
{
  LN_IntExpression expression;
  expression.kind = LN_IntExpressionKind::Constant;
  expression.int_value = value;
  return program.AddIntExpression(expression);
}

uint32_t AddConstantStringExpression(LN_Program &program, const std::string &value)
{
  LN_StringExpression expression;
  expression.kind = LN_StringExpressionKind::Constant;
  expression.string_value = value;
  return program.AddStringExpression(expression);
}

uint32_t AddConstantColorExpression(LN_Program &program, const MT_Vector4 &value)
{
  LN_ColorExpression expression;
  expression.kind = LN_ColorExpressionKind::Constant;
  expression.color_value = value;
  return program.AddColorExpression(expression);
}

uint32_t AddGamePropertyRef(LN_Program &program,
                            const std::string &name,
                            LN_ValueType value_type,
                            const LN_Value &default_value)
{
  LN_GamePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = value_type;
  property_ref.default_value = default_value;
  property_ref.default_value.type = value_type;
  return program.AddGamePropertyRef(property_ref);
}

uint32_t AddTreePropertyRef(LN_Program &program,
                            const std::string &name,
                            LN_ValueType value_type,
                            const LN_Value &default_value)
{
  const std::vector<LN_TreePropertyRef> &property_refs = program.GetTreePropertyRefs();
  for (uint32_t property_ref_index = 0; property_ref_index < property_refs.size();
       property_ref_index++)
  {
    if (property_refs[property_ref_index].name == name) {
      if (property_refs[property_ref_index].value_type == LN_ValueType::Generic &&
          value_type != LN_ValueType::Generic)
      {
        LN_TreePropertyRef &existing_ref =
            const_cast<std::vector<LN_TreePropertyRef> &>(property_refs)[property_ref_index];
        existing_ref.value_type = value_type;
        existing_ref.default_value = default_value;
        existing_ref.default_value.type = value_type;
      }
      return property_ref_index;
    }
  }

  LN_TreePropertyRef property_ref;
  property_ref.name = name;
  property_ref.value_type = value_type;
  property_ref.default_value = default_value;
  property_ref.default_value.type = value_type;
  return program.AddTreePropertyRef(property_ref);
}

uint32_t AddConstantValueExpression(LN_Program &program, const LN_Value &value)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::Constant;
  expression.value = value;
  return program.AddValueExpression(expression);
}

uint32_t AddActiveCameraValueExpression(LN_Program &program)
{
  LN_ValueExpression expression;
  expression.kind = LN_ValueExpressionKind::ActiveCamera;
  return program.AddValueExpression(expression);
}

uint32_t AddNotBoolExpression(LN_Program &program, uint32_t input)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Not;
  expression.input0 = input;
  return program.AddBoolExpression(expression);
}

uint32_t AddAndBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::And;
  expression.input0 = input0;
  expression.input1 = input1;
  return program.AddBoolExpression(expression);
}

uint32_t AddOrBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::Or;
  expression.input0 = input0;
  expression.input1 = input1;
  return program.AddBoolExpression(expression);
}

uint32_t AddXorBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1)
{
  const uint32_t either = AddOrBoolExpression(program, input0, input1);
  const uint32_t both = AddAndBoolExpression(program, input0, input1);
  return AddAndBoolExpression(program, either, AddNotBoolExpression(program, both));
}

std::optional<uint32_t> AddLogicGateBoolExpression(LN_Program &program,
                                                  const int16_t operation,
                                                  const uint32_t input0,
                                                  const uint32_t input1)
{
  switch (operation) {
    case 0:
      return AddAndBoolExpression(program, input0, input1);
    case 1:
      return AddOrBoolExpression(program, input0, input1);
    case 2:
      return AddXorBoolExpression(program, input0, input1);
    case 3:
      return AddNotBoolExpression(program, input0);
    case 4:
      return AddNotBoolExpression(program, AddAndBoolExpression(program, input0, input1));
    case 5:
      return AddNotBoolExpression(program, AddOrBoolExpression(program, input0, input1));
    case 6:
      return AddNotBoolExpression(program, AddXorBoolExpression(program, input0, input1));
    case 7:
      return AddAndBoolExpression(program, input0, AddNotBoolExpression(program, input1));
    default:
      return std::nullopt;
  }
}

uint32_t AddFloatCompareBoolExpression(LN_Program &program,
                                       uint32_t input0,
                                       uint32_t input1,
                                       LN_FloatCompareOperation operation)
{
  LN_BoolExpression expression;
  expression.kind = LN_BoolExpressionKind::FloatCompare;
  expression.input0 = input0;
  expression.input1 = input1;
  expression.float_compare_operation = operation;
  return program.AddBoolExpression(expression);
}

float FloatExpressionConstantFallback(const LN_Program &program, uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return 0.0f;
  }

  const std::vector<LN_FloatExpression> &expressions = program.GetFloatExpressions();
  if (expression_index >= expressions.size()) {
    return 0.0f;
  }

  const LN_FloatExpression &expression = expressions[expression_index];
  if (expression.kind == LN_FloatExpressionKind::Constant) {
    return expression.float_value;
  }
  return 0.0f;
}

bool BoolExpressionConstantFallback(const LN_Program &program, uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return false;
  }

  const std::vector<LN_BoolExpression> &expressions = program.GetBoolExpressions();
  if (expression_index >= expressions.size()) {
    return false;
  }

  const LN_BoolExpression &expression = expressions[expression_index];
  return expression.kind == LN_BoolExpressionKind::Constant ? expression.bool_value : false;
}

int32_t IntExpressionConstantFallback(const LN_Program &program, uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return 0;
  }

  const std::vector<LN_IntExpression> &expressions = program.GetIntExpressions();
  if (expression_index >= expressions.size()) {
    return 0;
  }

  const LN_IntExpression &expression = expressions[expression_index];
  return expression.kind == LN_IntExpressionKind::Constant ? expression.int_value : 0;
}

uint32_t AddConstantVectorExpression(LN_Program &program, const MT_Vector3 &value)
{
  LN_VectorExpression expression;
  expression.kind = LN_VectorExpressionKind::Constant;
  expression.vector_value = value;
  return program.AddVectorExpression(expression);
}

uint32_t AddVectorComponentFloatExpression(LN_Program &program,
                                           uint32_t vector_expression_index,
                                           uint8_t component_index)
{
  LN_FloatExpression expression;
  expression.kind = LN_FloatExpressionKind::VectorComponent;
  expression.input0 = vector_expression_index;
  expression.component_index = component_index;
  return program.AddFloatExpression(expression);
}

MT_Vector3 VectorExpressionConstantFallback(const LN_Program &program, uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  const std::vector<LN_VectorExpression> &expressions = program.GetVectorExpressions();
  if (expression_index >= expressions.size()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }

  const LN_VectorExpression &expression = expressions[expression_index];
  if (expression.kind == LN_VectorExpressionKind::Constant) {
    return expression.vector_value;
  }
  return MT_Vector3(0.0f, 0.0f, 0.0f);
}

MT_Vector4 ColorExpressionConstantFallback(const LN_Program &program, uint32_t expression_index)
{
  if (expression_index == LN_INVALID_INDEX) {
    return MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  const std::vector<LN_ColorExpression> &expressions = program.GetColorExpressions();
  if (expression_index >= expressions.size()) {
    return MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  const LN_ColorExpression &expression = expressions[expression_index];
  if (expression.kind == LN_ColorExpressionKind::Constant) {
    return expression.color_value;
  }
  return MT_Vector4(0.0f, 0.0f, 0.0f, 1.0f);
}

std::optional<uint32_t> BuildMouseOverQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  IntExpressionCache int_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;
  LN_QueryExpression expression;
  expression.kind = LN_QueryExpressionKind::MouseOver;
  const std::optional<uint32_t> object_expr = BuildOptionalObjectTargetExpression(
      program,
      node,
      "Object",
      node_definitions,
      input_links,
      value_cache,
      bool_expression_cache,
      int_expression_cache,
      float_expression_cache,
      string_expression_cache,
      vector_expression_cache,
      color_expression_cache,
      value_expression_cache);
  if (object_expr) {
    expression.input0 = *object_expr;
  }
  expression.cache_key = node.identifier;
  return program.AddQueryExpression(expression);
}

static std::optional<uint32_t> BuildOptionalExecutionCondition(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return AddConstantBoolExpression(program, true);
  }
  const auto links_iter = input_links.find(socket);
  if (links_iter == input_links.end() || links_iter->second.empty()) {
    return AddConstantBoolExpression(program, true);
  }
  return BuildInputExecutionExpression(program,
                                       node,
                                       socket_name,
                                       node_definitions,
                                       input_links,
                                       value_cache,
                                       float_expression_cache,
                                       bool_expression_cache);
}

static std::optional<uint32_t> BuildRaycastQueryExpressionWithKind(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache,
    const LN_QueryExpressionKind kind)
{
  IntExpressionCache int_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;

  const std::optional<uint32_t> condition_expr = BuildOptionalExecutionCondition(program,
                                                                                 node,
                                                                                 "Flow",
                                                                                 node_definitions,
                                                                                 input_links,
                                                                                 value_cache,
                                                                                 float_expression_cache,
                                                                                 bool_expression_cache);
  const std::optional<uint32_t> caster_expr = BuildOptionalObjectTargetExpression(
      program,
      node,
      "Caster",
      node_definitions,
      input_links,
      value_cache,
      bool_expression_cache,
      int_expression_cache,
      float_expression_cache,
      string_expression_cache,
      vector_expression_cache,
      color_expression_cache,
      value_expression_cache);
  const std::optional<uint32_t> ignore_object_expr = BuildInputValueExpression(
      program,
      node,
      "Ignore Object",
      node_definitions,
      input_links,
      value_cache,
      bool_expression_cache,
      int_expression_cache,
      float_expression_cache,
      string_expression_cache,
      vector_expression_cache,
      color_expression_cache,
      value_expression_cache);
  const std::optional<uint32_t> origin_expr = BuildInputVectorExpression(program,
                                                                         node,
                                                                         "Origin",
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         vector_expression_cache);
  const std::optional<uint32_t> destination_expr = BuildInputVectorExpression(
      program,
      node,
      "Destination",
      node_definitions,
      input_links,
      value_cache,
      float_expression_cache,
      vector_expression_cache);
  const std::optional<uint32_t> direction_expr = BuildInputVectorExpression(
      program,
      node,
      "Direction",
      node_definitions,
      input_links,
      value_cache,
      float_expression_cache,
      vector_expression_cache);
  const std::optional<uint32_t> max_distance_expr = BuildInputFloatExpression(
      program,
      node,
      "Max Distance",
      node_definitions,
      input_links,
      value_cache,
      float_expression_cache);
  const std::optional<uint32_t> local_expr = BuildInputBoolExpression(program,
                                                                      node,
                                                                      "Local",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      bool_expression_cache);
  const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Property",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  const std::optional<uint32_t> xray_expr = BuildInputBoolExpression(program,
                                                                     node,
                                                                     "X-Ray",
                                                                     node_definitions,
                                                                     input_links,
                                                                     value_cache,
                                                                     float_expression_cache,
                                                                     bool_expression_cache);
  const std::optional<uint32_t> mask_expr = BuildInputIntExpression(program,
                                                                    node,
                                                                    "Mask",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    int_expression_cache);
  const std::optional<uint32_t> include_sensors_expr = BuildInputBoolExpression(
      program,
      node,
      "Include Sensors",
      node_definitions,
      input_links,
      value_cache,
      float_expression_cache,
      bool_expression_cache);
  const std::optional<uint32_t> hit_back_faces_expr = BuildInputBoolExpression(
      program,
      node,
      "Hit Backfaces",
      node_definitions,
      input_links,
      value_cache,
      float_expression_cache,
      bool_expression_cache);
  std::optional<uint32_t> max_results_expr = AddConstantIntExpression(program, 1);
  if (kind == LN_QueryExpressionKind::RaycastAll) {
    max_results_expr = BuildInputIntExpression(program,
                                               node,
                                               "Max Results",
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               int_expression_cache);
  }
  std::optional<uint32_t> visualize_expr;
  if (FindInputSocket(node, "Visualize") != nullptr) {
    visualize_expr = BuildInputBoolExpression(program,
                                              node,
                                              "Visualize",
                                              node_definitions,
                                              input_links,
                                              value_cache,
                                              float_expression_cache,
                                              bool_expression_cache);
  }
  else {
    visualize_expr = AddConstantBoolExpression(program, false);
  }
  if (!condition_expr || !ignore_object_expr || !origin_expr || !destination_expr ||
      !direction_expr || !max_distance_expr || !local_expr || !property_expr || !xray_expr ||
      !mask_expr || !include_sensors_expr || !hit_back_faces_expr || !max_results_expr ||
      !visualize_expr)
  {
    return std::nullopt;
  }

  LN_QueryExpression expression;
  expression.kind = kind;
  expression.condition_bool_expr_index = *condition_expr;
  if (caster_expr) {
    expression.input0 = *caster_expr;
  }
  expression.input1 = *origin_expr;
  expression.input2 = *destination_expr;
  expression.input3 = *ignore_object_expr;
  expression.input4 = *direction_expr;
  expression.bool_expr_index = *local_expr;
  expression.secondary_bool_expr_index = *xray_expr;
  expression.tertiary_bool_expr_index = *visualize_expr;
  expression.quaternary_bool_expr_index = *include_sensors_expr;
  expression.quinary_bool_expr_index = *hit_back_faces_expr;
  expression.int_expr_index = *mask_expr;
  expression.secondary_int_expr_index = *max_results_expr;
  expression.float_expr_index = *max_distance_expr;
  expression.string_expr_index = *property_expr;
  expression.vector_value = VectorExpressionConstantFallback(program, *origin_expr);
  expression.secondary_vector_value = VectorExpressionConstantFallback(program, *destination_expr);
  expression.tertiary_vector_value = VectorExpressionConstantFallback(program, *direction_expr);
  expression.float_value = FloatExpressionConstantFallback(program, *max_distance_expr);
  expression.bool_value = BoolExpressionConstantFallback(program, *local_expr);
  expression.int_value = IntExpressionConstantFallback(program, *mask_expr);
  expression.secondary_int_value = IntExpressionConstantFallback(program, *max_results_expr);
  expression.ray_input_mode = LN_RayInputMode(std::clamp<int>(node.custom1, 0, 1));
  expression.cache_key = node.identifier;
  return program.AddQueryExpression(expression);
}

std::optional<uint32_t> BuildRaycastQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  return BuildRaycastQueryExpressionWithKind(program,
                                             node,
                                             node_definitions,
                                             input_links,
                                             value_cache,
                                             float_expression_cache,
                                             bool_expression_cache,
                                             LN_QueryExpressionKind::Raycast);
}

std::optional<uint32_t> BuildRaycastAllQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  return BuildRaycastQueryExpressionWithKind(program,
                                             node,
                                             node_definitions,
                                             input_links,
                                             value_cache,
                                             float_expression_cache,
                                             bool_expression_cache,
                                             LN_QueryExpressionKind::RaycastAll);
}

static std::optional<uint32_t> BuildShapeCastQueryExpressionWithKind(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_cache,
    BoolExpressionCache &bool_cache,
    const LN_QueryExpressionKind kind)
{
  IntExpressionCache int_cache;
  StringExpressionCache string_cache;
  VectorExpressionCache vector_cache;
  ColorExpressionCache color_cache;
  ValueExpressionCache value_expr_cache;

  const auto condition = BuildOptionalExecutionCondition(
      program, node, "Flow", node_definitions, input_links, value_cache, float_cache, bool_cache);
  const auto caster = BuildOptionalObjectTargetExpression(program,
                                                           node,
                                                           "Caster",
                                                           node_definitions,
                                                           input_links,
                                                           value_cache,
                                                           bool_cache,
                                                           int_cache,
                                                           float_cache,
                                                           string_cache,
                                                           vector_cache,
                                                           color_cache,
                                                           value_expr_cache);
  const auto ignore = BuildInputValueExpression(program,
                                                node,
                                                "Ignore Object",
                                                node_definitions,
                                                input_links,
                                                value_cache,
                                                bool_cache,
                                                int_cache,
                                                float_cache,
                                                string_cache,
                                                vector_cache,
                                                color_cache,
                                                value_expr_cache);
  const auto origin = BuildInputVectorExpression(
      program, node, "Origin", node_definitions, input_links, value_cache, float_cache, vector_cache);
  const auto destination = BuildInputVectorExpression(program,
                                                      node,
                                                      "Destination",
                                                      node_definitions,
                                                      input_links,
                                                      value_cache,
                                                      float_cache,
                                                      vector_cache);
  const auto rotation = BuildInputRotationVectorExpression(program,
                                                           node,
                                                           "Rotation",
                                                           node_definitions,
                                                           input_links,
                                                           value_cache,
                                                           float_cache,
                                                           vector_cache);
  const auto half_extents = BuildInputVectorExpression(program,
                                                       node,
                                                       "Half Extents",
                                                       node_definitions,
                                                       input_links,
                                                       value_cache,
                                                       float_cache,
                                                       vector_cache);
  const auto radius = BuildInputFloatExpression(
      program, node, "Radius", node_definitions, input_links, value_cache, float_cache);
  const auto height = BuildInputFloatExpression(
      program, node, "Height", node_definitions, input_links, value_cache, float_cache);
  const auto extra_radius = BuildInputFloatExpression(
      program, node, "Extra Radius", node_definitions, input_links, value_cache, float_cache);
  const auto local = BuildInputBoolExpression(
      program, node, "Local", node_definitions, input_links, value_cache, float_cache, bool_cache);
  const auto property = BuildInputStringExpression(
      program, node, "Property", node_definitions, input_links, value_cache, string_cache);
  const auto xray = BuildInputBoolExpression(
      program, node, "X-Ray", node_definitions, input_links, value_cache, float_cache, bool_cache);
  const auto mask = BuildInputIntExpression(
      program, node, "Mask", node_definitions, input_links, value_cache, int_cache);
  const auto sensors = BuildInputBoolExpression(program,
                                                node,
                                                "Include Sensors",
                                                node_definitions,
                                                input_links,
                                                value_cache,
                                                float_cache,
                                                bool_cache);
  const auto backfaces = BuildInputBoolExpression(program,
                                                  node,
                                                  "Hit Backfaces",
                                                  node_definitions,
                                                  input_links,
                                                  value_cache,
                                                  float_cache,
                                                  bool_cache);
  const auto visualize = BuildInputBoolExpression(program,
                                                  node,
                                                  "Visualize",
                                                  node_definitions,
                                                  input_links,
                                                  value_cache,
                                                  float_cache,
                                                  bool_cache);
  std::optional<uint32_t> max_results = AddConstantIntExpression(program, 1);
  if (kind == LN_QueryExpressionKind::ShapeCastAll) {
    max_results = BuildInputIntExpression(
        program, node, "Max Results", node_definitions, input_links, value_cache, int_cache);
  }

  if (!condition || !ignore || !origin || !destination || !rotation || !half_extents ||
      !radius || !height || !extra_radius || !local || !property || !xray || !mask ||
      !sensors || !backfaces || !visualize || !max_results)
  {
    return std::nullopt;
  }

  LN_QueryExpression expression;
  expression.kind = kind;
  expression.condition_bool_expr_index = *condition;
  expression.input0 = caster.value_or(LN_INVALID_INDEX);
  expression.input1 = *origin;
  expression.input2 = *destination;
  expression.input3 = *ignore;
  expression.input4 = *rotation;
  expression.input5 = *half_extents;
  expression.float_expr_index = *radius;
  expression.secondary_float_expr_index = *height;
  expression.tertiary_float_expr_index = *extra_radius;
  expression.bool_expr_index = *local;
  expression.secondary_bool_expr_index = *xray;
  expression.tertiary_bool_expr_index = *visualize;
  expression.quaternary_bool_expr_index = *sensors;
  expression.quinary_bool_expr_index = *backfaces;
  expression.int_expr_index = *mask;
  expression.secondary_int_expr_index = *max_results;
  expression.string_expr_index = *property;
  expression.vector_value = VectorExpressionConstantFallback(program, *origin);
  expression.secondary_vector_value = VectorExpressionConstantFallback(program, *destination);
  expression.tertiary_vector_value = VectorExpressionConstantFallback(program, *rotation);
  expression.quaternary_vector_value = VectorExpressionConstantFallback(program, *half_extents);
  expression.float_value = FloatExpressionConstantFallback(program, *radius);
  expression.secondary_float_value = FloatExpressionConstantFallback(program, *height);
  expression.tertiary_float_value = FloatExpressionConstantFallback(program, *extra_radius);
  expression.bool_value = BoolExpressionConstantFallback(program, *local);
  expression.int_value = IntExpressionConstantFallback(program, *mask);
  expression.secondary_int_value = IntExpressionConstantFallback(program, *max_results);
  expression.shape_cast_type = LN_ShapeCastType(std::clamp<int>(node.custom1, 0, 2));
  expression.cache_key = node.identifier;
  return program.AddQueryExpression(expression);
}

std::optional<uint32_t> BuildShapeCastQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_cache,
    BoolExpressionCache &bool_cache)
{
  return BuildShapeCastQueryExpressionWithKind(program,
                                               node,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               float_cache,
                                               bool_cache,
                                               LN_QueryExpressionKind::ShapeCast);
}

std::optional<uint32_t> BuildShapeCastAllQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_cache,
    BoolExpressionCache &bool_cache)
{
  return BuildShapeCastQueryExpressionWithKind(program,
                                               node,
                                               node_definitions,
                                               input_links,
                                               value_cache,
                                               float_cache,
                                               bool_cache,
                                               LN_QueryExpressionKind::ShapeCastAll);
}

std::optional<uint32_t> BuildMouseRayQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  IntExpressionCache int_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return std::nullopt;
  }
  const std::optional<uint32_t> condition_expr =
      BuildPrimaryExecutionExpression(program,
                                                        node,
                                                        *definition_iter->second,
                                                        node_definitions,
                                                        input_links,
                                                        value_cache,
                                                        float_expression_cache,
                                                        bool_expression_cache);
  const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Property",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  const std::optional<uint32_t> xray_expr = BuildInputBoolExpression(program,
                                                                     node,
                                                                     "X-Ray",
                                                                     node_definitions,
                                                                     input_links,
                                                                     value_cache,
                                                                     float_expression_cache,
                                                                     bool_expression_cache);
  const std::optional<uint32_t> distance_expr = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Distance",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
  const std::optional<uint32_t> mask_expr = BuildInputIntExpression(program,
                                                                    node,
                                                                    "Mask",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    int_expression_cache);
  if (!condition_expr || !property_expr || !xray_expr || !distance_expr || !mask_expr) {
    return std::nullopt;
  }

  LN_QueryExpression expression;
  expression.kind = LN_QueryExpressionKind::MouseRay;
  expression.condition_bool_expr_index = *condition_expr;
  expression.string_expr_index = *property_expr;
  expression.bool_expr_index = *xray_expr;
  expression.float_expr_index = *distance_expr;
  expression.int_expr_index = *mask_expr;
  expression.float_value = FloatExpressionConstantFallback(program, *distance_expr);
  expression.bool_value = BoolExpressionConstantFallback(program, *xray_expr);
  expression.int_value = IntExpressionConstantFallback(program, *mask_expr);
  expression.cache_key = node.identifier;
  return program.AddQueryExpression(expression);
}

std::optional<uint32_t> BuildCameraRayQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  IntExpressionCache int_expression_cache;
  StringExpressionCache string_expression_cache;
  VectorExpressionCache vector_expression_cache;
  ColorExpressionCache color_expression_cache;
  ValueExpressionCache value_expression_cache;

  const auto definition_iter = node_definitions.find(&node);
  if (definition_iter == node_definitions.end()) {
    return std::nullopt;
  }
  const std::optional<uint32_t> condition_expr =
      BuildPrimaryExecutionExpression(program,
                                                        node,
                                                        *definition_iter->second,
                                                        node_definitions,
                                                        input_links,
                                                        value_cache,
                                                        float_expression_cache,
                                                        bool_expression_cache);
  const std::optional<uint32_t> aim_expr = BuildInputVectorExpression(program,
                                                                      node,
                                                                      "Aim",
                                                                      node_definitions,
                                                                      input_links,
                                                                      value_cache,
                                                                      float_expression_cache,
                                                                      vector_expression_cache);
  const std::optional<uint32_t> property_expr = BuildInputStringExpression(program,
                                                                           node,
                                                                           "Property",
                                                                           node_definitions,
                                                                           input_links,
                                                                           value_cache,
                                                                           string_expression_cache);
  const std::optional<uint32_t> xray_expr = BuildInputBoolExpression(program,
                                                                     node,
                                                                     "X-Ray",
                                                                     node_definitions,
                                                                     input_links,
                                                                     value_cache,
                                                                     float_expression_cache,
                                                                     bool_expression_cache);
  const std::optional<uint32_t> distance_expr = BuildInputFloatExpression(program,
                                                                          node,
                                                                          "Distance",
                                                                          node_definitions,
                                                                          input_links,
                                                                          value_cache,
                                                                          float_expression_cache);
  const std::optional<uint32_t> mask_expr = BuildInputIntExpression(program,
                                                                    node,
                                                                    "Mask",
                                                                    node_definitions,
                                                                    input_links,
                                                                    value_cache,
                                                                    int_expression_cache);
  if (!condition_expr || !aim_expr || !property_expr || !xray_expr || !distance_expr || !mask_expr) {
    return std::nullopt;
  }

  LN_QueryExpression expression;
  expression.kind = LN_QueryExpressionKind::CameraRay;
  expression.condition_bool_expr_index = *condition_expr;
  expression.input1 = *aim_expr;
  expression.string_expr_index = *property_expr;
  expression.bool_expr_index = *xray_expr;
  expression.float_expr_index = *distance_expr;
  expression.int_expr_index = *mask_expr;
  expression.vector_value = VectorExpressionConstantFallback(program, *aim_expr);
  expression.float_value = FloatExpressionConstantFallback(program, *distance_expr);
  expression.bool_value = BoolExpressionConstantFallback(program, *xray_expr);
  expression.int_value = IntExpressionConstantFallback(program, *mask_expr);
  expression.cache_key = node.identifier;
  return program.AddQueryExpression(expression);
}

std::optional<uint32_t> BuildInputBoolExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    const auto definition_iter = node_definitions.find(link.fromnode);
    if (definition_iter == node_definitions.end()) {
      return std::nullopt;
    }
    const LN_PinDefinition *pin = FindPinDefinition(definition_iter->second->outputs,
                                                    *link.fromsock);
    if (pin == nullptr || pin->kind == LN_PinKind::Execution) {
      return std::nullopt;
    }
    return BuildOutputBoolExpression(program,
                                     *link.fromnode,
                                     *link.fromsock,
                                     node_definitions,
                                     input_links,
                                     value_cache,
                                     float_expression_cache,
                                     bool_expression_cache);
  }

  const std::optional<LN_Value> default_value = ReadSocketDefault(*socket, LN_ValueType::Bool);
  if (!default_value || default_value->type != LN_ValueType::Bool) {
    return std::nullopt;
  }
  return AddConstantBoolExpression(program, default_value->bool_value);
}

std::optional<uint32_t> BuildInputExecutionExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache)
{
  const blender::bNodeSocket *socket = FindInputSocket(node, socket_name);
  if (socket == nullptr) {
    return std::nullopt;
  }

  const auto input_definition_iter = node_definitions.find(&node);
  if (input_definition_iter != node_definitions.end()) {
    const LN_PinDefinition *input_pin = FindPinDefinition(input_definition_iter->second->inputs,
                                                          *socket);
    if (input_pin == nullptr || input_pin->kind != LN_PinKind::Execution) {
      return std::nullopt;
    }
  }

  const auto links_iter = input_links.find(socket);
  if (links_iter != input_links.end() && !links_iter->second.empty()) {
    const ResolvedLink &link = links_iter->second.front();
    const auto definition_iter = node_definitions.find(link.fromnode);
    if (definition_iter == node_definitions.end()) {
      return std::nullopt;
    }

    const LN_NodeDefinition &upstream_definition = *definition_iter->second;
    const LN_PinDefinition *upstream_pin = FindPinDefinition(upstream_definition.outputs,
                                                            *link.fromsock);
    if (upstream_pin == nullptr || upstream_pin->kind != LN_PinKind::Execution) {
      return std::nullopt;
    }

    const std::optional<uint32_t> expression = BuildOutputBoolExpression(program,
                                                                         *link.fromnode,
                                                                         *link.fromsock,
                                                                         node_definitions,
                                                                         input_links,
                                                                         value_cache,
                                                                         float_expression_cache,
                                                                         bool_expression_cache);
    if (expression) {
      return expression;
    }

    const InternalCompileHandler *handler = FindInternalCompileHandler(definition_iter->second->kind);
    if (handler != nullptr && handler->descriptor.info.emits_commands &&
        (NamesMatch(link.fromsock->name, link.fromsock->identifier, "Done") ||
         NamesMatch(link.fromsock->name, link.fromsock->identifier, "On Start") ||
         NamesMatch(link.fromsock->name, link.fromsock->identifier, "When Done")))
    {
      const std::optional<uint32_t> command_condition =
          BuildPrimaryExecutionExpression(program,
                                          *link.fromnode,
                                          upstream_definition,
                                          node_definitions,
                                          input_links,
                                          value_cache,
                                          float_expression_cache,
                                          bool_expression_cache);
      if (command_condition) {
        return command_condition;
      }
    }

    switch (definition_iter->second->kind) {
      case LN_NodeKind::EventOnInit:
      case LN_NodeKind::EventOnFixedUpdate:
      case LN_NodeKind::Branch:
        return AddConstantBoolExpression(program, true);
      default:
        break;
    }
    return std::nullopt;
  }

  return AddConstantBoolExpression(program, false);
}

}  // namespace ln_compiler
