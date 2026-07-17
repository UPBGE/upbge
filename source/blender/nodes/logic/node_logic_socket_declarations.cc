/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_logic_socket_declarations.hh"

#include "NOD_logic.hh"

#include <algorithm>

#include "BKE_node.hh"

#include "BLI_math_vector.h"
#include "BLI_string.h"

namespace blender::nodes::logic::decl {

static constexpr const char *execution_socket_idname = "NodeSocketLogicExecution";
static constexpr const char *condition_socket_idname = "NodeSocketLogicCondition";
static constexpr const char *bool_socket_idname = "NodeSocketLogicBool";
static constexpr const char *int_socket_idname = "NodeSocketLogicInt";
static constexpr const char *collision_layers_socket_idname = "NodeSocketLogicCollisionLayers";
static constexpr const char *old_collision_layers_socket_idname =
    "NodeSocketLogicCollisionCollections";
static constexpr const char *float_socket_idname = "NodeSocketLogicFloat";
static constexpr const char *string_socket_idname = "NodeSocketLogicString";
static constexpr const char *vector_socket_idname = "NodeSocketLogicVector";
static constexpr const char *vector_xy_angle_socket_idname = "NodeSocketLogicVectorXYAngle";

static const char *logic_vector_socket_idname(const nodes::decl::Vector &decl)
{
  if (decl.dimensions == 2 && decl.subtype == PROP_EULER) {
    return vector_xy_angle_socket_idname;
  }
  return vector_socket_idname;
}
static constexpr const char *rotation_socket_idname = "NodeSocketLogicRotation";
static constexpr const char *color_socket_idname = "NodeSocketLogicColor";
static constexpr const char *generic_socket_idname = "NodeSocketLogicGeneric";
static constexpr const char *list_socket_idname = "NodeSocketLogicList";
static constexpr const char *dictionary_socket_idname = "NodeSocketLogicDictionary";
static constexpr const char *geometry_tree_socket_idname = "NodeSocketLogicGeometryTree";
static constexpr const char *mesh_socket_idname = "NodeSocketLogicMesh";
static constexpr const char *datablock_socket_idname = "NodeSocketLogicDatablock";
static constexpr const char *ui_socket_idname = "NodeSocketLogicUI";

static bool socket_has_idname(const bNodeSocket &socket, const StringRef idname)
{
  return StringRef(socket.idname) == idname;
}

static bool sockets_can_connect(const SocketDeclaration &socket_decl, const bNodeSocket &socket)
{
  return socket_decl.in_out != socket.in_out;
}

static bool socket_is_boolean_compatible(const bNodeSocket &socket)
{
  if (socket.type != SOCK_BOOLEAN) {
    return false;
  }
  return STREQ(socket.idname, condition_socket_idname) || STREQ(socket.idname, bool_socket_idname) ||
         STREQ(socket.idname, "NodeSocketBool");
}

static bool socket_is_execution_compatible(const bNodeSocket &socket)
{
  return socket.type == SOCK_BOOLEAN && STREQ(socket.idname, execution_socket_idname);
}

static bool condition_socket_matches_common_data(const Condition &decl,
                                                 const bNodeSocket &socket)
{
  if (StringRef(socket.name) != decl.name.ref()) {
    return false;
  }
  if (StringRef(socket.identifier) != decl.identifier.ref()) {
    return false;
  }
  if (((socket.flag & SOCK_HIDE_VALUE) != 0) != decl.hide_value) {
    return false;
  }
  if (((socket.flag & SOCK_MULTI_INPUT) != 0) != decl.is_multi_input) {
    return false;
  }
  if (((socket.flag & SOCK_UNAVAIL) != 0) != !decl.is_available) {
    return false;
  }
  return true;
}

bNodeSocket &Execution::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             execution_socket_idname,
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  socket.flag |= SOCK_HIDE_VALUE;
  bNodeSocketValueBoolean &value = *static_cast<bNodeSocketValueBoolean *>(socket.default_value);
  value.value = this->default_value;
  return socket;
}

bool Execution::matches(const bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, execution_socket_idname)) {
    return false;
  }
  if (StringRef(socket.name) != this->name.ref()) {
    return false;
  }
  if (StringRef(socket.identifier) != this->identifier.ref()) {
    return false;
  }
  if ((socket.flag & SOCK_HIDE_VALUE) == 0) {
    return false;
  }
  if (((socket.flag & SOCK_MULTI_INPUT) != 0) != this->is_multi_input) {
    return false;
  }
  if (((socket.flag & SOCK_UNAVAIL) != 0) != !this->is_available) {
    return false;
  }
  return true;
}

bNodeSocket &Execution::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, execution_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (StringRef(socket.identifier) != this->identifier.ref()) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (StringRef(socket.name) != this->name.ref()) {
    STRNCPY(socket.name, this->name.c_str());
  }
  this->set_common_flags(socket);
  socket.flag |= SOCK_HIDE_VALUE;
  return socket;
}

bool Execution::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket_is_execution_compatible(socket);
}

bNodeSocket &Condition::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             condition_socket_idname,
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueBoolean &value = *static_cast<bNodeSocketValueBoolean *>(socket.default_value);
  value.value = this->default_value;
  return socket;
}

bool Condition::matches(const bNodeSocket &socket) const
{
  return condition_socket_matches_common_data(*this, socket) &&
         socket_has_idname(socket, condition_socket_idname);
}

bNodeSocket &Condition::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, condition_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (StringRef(socket.identifier) != this->identifier.ref()) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (StringRef(socket.name) != this->name.ref()) {
    STRNCPY(socket.name, this->name.c_str());
  }
  this->set_common_flags(socket);
  return socket;
}

bool Condition::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket_is_boolean_compatible(socket);
}

bNodeSocket &Bool::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             bool_socket_idname,
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueBoolean &value = *static_cast<bNodeSocketValueBoolean *>(socket.default_value);
  value.value = this->default_value;
  return socket;
}

bool Bool::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, bool_socket_idname);
}

bNodeSocket &Bool::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, bool_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Bool::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket_is_boolean_compatible(socket);
}

bNodeSocket &Int::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, int_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueInt &value = *static_cast<bNodeSocketValueInt *>(socket.default_value);
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.value = this->default_value;
  return socket;
}

bool Int::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, int_socket_idname);
}

bNodeSocket &Int::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, int_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  bNodeSocketValueInt &value = *static_cast<bNodeSocketValueInt *>(socket.default_value);
  value.subtype = this->subtype;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

bool Int::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_INT;
}

static void set_int_socket_value_range(const nodes::decl::Int &decl, bNodeSocket &socket)
{
  bNodeSocketValueInt &value = *static_cast<bNodeSocketValueInt *>(socket.default_value);
  value.subtype = decl.subtype;
  value.min = decl.soft_min_value;
  value.max = decl.soft_max_value;
  value.value = std::clamp(value.value, decl.soft_min_value, decl.soft_max_value);
}

bNodeSocket &CollisionLayers::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             collision_layers_socket_idname,
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueInt &value = *static_cast<bNodeSocketValueInt *>(socket.default_value);
  value.subtype = this->subtype;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.value = std::clamp(this->default_value, this->soft_min_value, this->soft_max_value);
  return socket;
}

bool CollisionLayers::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) &&
         (socket_has_idname(socket, collision_layers_socket_idname) ||
          socket_has_idname(socket, old_collision_layers_socket_idname));
}

bNodeSocket &CollisionLayers::update_or_build(bNodeTree &ntree,
                                              bNode &node,
                                              bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, collision_layers_socket_idname)) {
    if ((socket_has_idname(socket, int_socket_idname) ||
         socket_has_idname(socket, old_collision_layers_socket_idname)) &&
        socket.type == SOCK_INT)
    {
      bke::node_modify_socket_type(ntree, node, socket, collision_layers_socket_idname);
    }
    else {
      BLI_assert(socket.in_out == this->in_out);
      return this->build(ntree, node);
    }
  }
  this->set_common_flags(socket);
  set_int_socket_value_range(*this, socket);
  return socket;
}

bNodeSocket &Float::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, float_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueFloat &value = *static_cast<bNodeSocketValueFloat *>(socket.default_value);
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.value = this->default_value;
  return socket;
}

bool Float::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, float_socket_idname);
}

bNodeSocket &Float::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, float_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  bNodeSocketValueFloat &value = *static_cast<bNodeSocketValueFloat *>(socket.default_value);
  value.subtype = this->subtype;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

bool Float::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_FLOAT;
}

bNodeSocket &String::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, string_socket_idname, this->identifier.ref(), this->name.ref());
  STRNCPY((static_cast<bNodeSocketValueString *>(socket.default_value))->value,
          this->default_value.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool String::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, string_socket_idname);
}

bNodeSocket &String::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, string_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  bNodeSocketValueString &value = *static_cast<bNodeSocketValueString *>(socket.default_value);
  value.subtype = this->subtype;
  return socket;
}

bool String::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_STRING;
}

bNodeSocket &Vector::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             logic_vector_socket_idname(*this),
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueVector &value = *static_cast<bNodeSocketValueVector *>(socket.default_value);
  value.subtype = this->subtype;
  value.dimensions = this->dimensions;
  std::copy_n(&this->default_value[0], this->dimensions, value.value);
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

bool Vector::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket) ||
      !socket_has_idname(socket, logic_vector_socket_idname(*this)))
  {
    return false;
  }
  const bNodeSocketValueVector &value = *static_cast<const bNodeSocketValueVector *>(
      socket.default_value);
  return value.dimensions == this->dimensions && value.min == this->soft_min_value &&
         value.max == this->soft_max_value;
}

bNodeSocket &Vector::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, logic_vector_socket_idname(*this))) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  bNodeSocketValueVector &value = *static_cast<bNodeSocketValueVector *>(socket.default_value);
  value.subtype = this->subtype;
  value.dimensions = this->dimensions;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

bool Vector::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) &&
         (socket.type == SOCK_VECTOR || socket_has_idname(socket, list_socket_idname));
}

bNodeSocket &Rotation::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, rotation_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueRotation &value = *static_cast<bNodeSocketValueRotation *>(socket.default_value);
  copy_v3_v3(value.value_euler, float3(this->default_value));
  return socket;
}

bool Rotation::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, rotation_socket_idname);
}

bNodeSocket &Rotation::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, rotation_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Rotation::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_ROTATION;
}

bNodeSocket &Color::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, color_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  bNodeSocketValueRGBA &value = *static_cast<bNodeSocketValueRGBA *>(socket.default_value);
  copy_v4_v4(value.value, this->default_value);
  return socket;
}

bool Color::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, color_socket_idname);
}

bNodeSocket &Color::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, color_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Color::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_RGBA;
}

Object::Object() : nodes::decl::IDSocketDeclaration("NodeSocketLogicObject") {}

Scene::Scene() : nodes::decl::IDSocketDeclaration("NodeSocketLogicScene") {}

Collection::Collection() : nodes::decl::IDSocketDeclaration("NodeSocketLogicCollection") {}

Material::Material() : nodes::decl::IDSocketDeclaration("NodeSocketLogicMaterial") {}

Image::Image() : nodes::decl::IDSocketDeclaration("NodeSocketLogicImage") {}

Sound::Sound() : nodes::decl::IDSocketDeclaration("NodeSocketLogicSound") {}

Font::Font() : nodes::decl::IDSocketDeclaration("NodeSocketLogicFont") {}

Text::Text() : nodes::decl::IDSocketDeclaration("NodeSocketLogicText") {}

bNodeSocket &Generic::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, generic_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  return socket;
}

bool Generic::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, generic_socket_idname);
}

bNodeSocket &Generic::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, generic_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Generic::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket);
}

static bool custom_logic_socket_can_connect(const SocketDeclaration &declaration,
                                            const bNodeSocket &socket,
                                            const char *socket_idname)
{
  return sockets_can_connect(declaration, socket) &&
         (socket_has_idname(socket, socket_idname) ||
          socket_has_idname(socket, generic_socket_idname));
}

bNodeSocket &List::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, list_socket_idname, this->identifier.ref(), this->name.ref());
  this->set_common_flags(socket);
  return socket;
}

bool List::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, list_socket_idname);
}

bNodeSocket &List::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, list_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool List::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, list_socket_idname) ||
         (sockets_can_connect(*this, socket) && socket.type == SOCK_VECTOR);
}

bNodeSocket &Dictionary::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                             node,
                                             this->in_out,
                                             dictionary_socket_idname,
                                             this->identifier.ref(),
                                             this->name.ref());
  this->set_common_flags(socket);
  return socket;
}

bool Dictionary::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, dictionary_socket_idname);
}

bNodeSocket &Dictionary::update_or_build(bNodeTree &ntree,
                                         bNode &node,
                                         bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, dictionary_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Dictionary::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, dictionary_socket_idname);
}

static bNodeSocket &add_custom_logic_socket(const SocketDeclaration &declaration,
                                            bNodeTree &ntree,
                                            bNode &node,
                                            const char *socket_idname)
{
  return *bke::node_add_socket(ntree,
                               node,
                               declaration.in_out,
                               socket_idname,
                               declaration.identifier.ref(),
                               declaration.name.ref());
}

bNodeSocket &GeometryTree::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = add_custom_logic_socket(*this, ntree, node, geometry_tree_socket_idname);
  this->set_common_flags(socket);
  return socket;
}

bool GeometryTree::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) &&
         socket_has_idname(socket, geometry_tree_socket_idname);
}

bNodeSocket &GeometryTree::update_or_build(bNodeTree &ntree,
                                           bNode &node,
                                           bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, geometry_tree_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool GeometryTree::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, geometry_tree_socket_idname);
}

bNodeSocket &Mesh::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = add_custom_logic_socket(*this, ntree, node, mesh_socket_idname);
  this->set_common_flags(socket);
  return socket;
}

bool Mesh::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, mesh_socket_idname);
}

bNodeSocket &Mesh::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, mesh_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Mesh::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, mesh_socket_idname);
}

bNodeSocket &Datablock::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = add_custom_logic_socket(*this, ntree, node, datablock_socket_idname);
  this->set_common_flags(socket);
  return socket;
}

bool Datablock::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, datablock_socket_idname);
}

bNodeSocket &Datablock::update_or_build(bNodeTree &ntree,
                                        bNode &node,
                                        bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, datablock_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool Datablock::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, datablock_socket_idname);
}

bNodeSocket &UI::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = add_custom_logic_socket(*this, ntree, node, ui_socket_idname);
  this->set_common_flags(socket);
  return socket;
}

bool UI::matches(const bNodeSocket &socket) const
{
  return this->matches_common_data(socket) && socket_has_idname(socket, ui_socket_idname);
}

bNodeSocket &UI::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (!socket_has_idname(socket, ui_socket_idname)) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

bool UI::can_connect(const bNodeSocket &socket) const
{
  return custom_logic_socket_can_connect(*this, socket, ui_socket_idname);
}

}  // namespace blender::nodes::logic::decl
