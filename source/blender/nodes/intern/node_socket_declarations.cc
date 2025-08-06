/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_socket_declarations.hh"
#include "NOD_socket_declarations_geometry.hh"

#include "BKE_lib_id.hh"
#include "BKE_node_runtime.hh"

#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

namespace blender::nodes::decl {

/**
 * \note This function only deals with declarations, not the field status of existing nodes. If the
 * field status of existing nodes was stored on the sockets, an improvement would be to check the
 * existing socket's current status instead of the declaration.
 */
static bool field_types_are_compatible(const SocketDeclaration &input,
                                       const SocketDeclaration &output)
{
  if (output.output_field_dependency.field_type() == OutputSocketFieldType::FieldSource) {
    if (input.input_field_type == InputSocketFieldType::None) {
      return false;
    }
  }
  return true;
}

static bool sockets_can_connect(const SocketDeclaration &socket_decl,
                                const bNodeSocket &other_socket)
{
  /* Input sockets cannot connect to input sockets, outputs cannot connect to outputs. */
  if (socket_decl.in_out == other_socket.in_out) {
    return false;
  }

  if (other_socket.runtime->declaration) {
    if (socket_decl.in_out == SOCK_IN) {
      if (!field_types_are_compatible(socket_decl, *other_socket.runtime->declaration)) {
        return false;
      }
    }
    else {
      if (!field_types_are_compatible(*other_socket.runtime->declaration, socket_decl)) {
        return false;
      }
    }
  }

  return true;
}

static bool basic_types_can_connect(const SocketDeclaration & /*socket_decl*/,
                                    const bNodeSocket &other_socket)
{
  return ELEM(other_socket.type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA);
}

static void modify_subtype_except_for_storage(bNodeSocket &socket, int new_subtype)
{
  const StringRefNull idname = *bke::node_static_socket_type(socket.type, new_subtype);
  STRNCPY_UTF8(socket.idname, idname.c_str());
  bke::bNodeSocketType *socktype = bke::node_socket_type_find(idname);
  socket.typeinfo = socktype;
}

static void modify_subtype_except_for_storage(bNodeSocket &socket, int subtype, int dimensions)
{
  const StringRefNull idname = *bke::node_static_socket_type(socket.type, subtype, dimensions);
  STRNCPY_UTF8(socket.idname, idname.c_str());
  bke::bNodeSocketType *socktype = bke::node_socket_type_find(idname);
  socket.typeinfo = socktype;
}

/* -------------------------------------------------------------------- */
/** \name #Float
 * \{ */

bNodeSocket &Float::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_FLOAT,
                                                     this->subtype,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueFloat &value = *(bNodeSocketValueFloat *)socket.default_value;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.value = this->default_value;
  return socket;
}

bool Float::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_FLOAT) {
    return false;
  }
  if (socket.typeinfo->subtype != this->subtype) {
    return false;
  }
  bNodeSocketValueFloat &value = *(bNodeSocketValueFloat *)socket.default_value;
  if (value.min != this->soft_min_value) {
    return false;
  }
  if (value.max != this->soft_max_value) {
    return false;
  }
  return true;
}

bool Float::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  if (this->in_out == SOCK_OUT && socket.type == SOCK_ROTATION) {
    return true;
  }
  return basic_types_can_connect(*this, socket);
}

bNodeSocket &Float::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_FLOAT) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (socket.typeinfo->subtype != this->subtype) {
    modify_subtype_except_for_storage(socket, this->subtype);
  }
  this->set_common_flags(socket);
  bNodeSocketValueFloat &value = *(bNodeSocketValueFloat *)socket.default_value;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.subtype = this->subtype;
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Int
 * \{ */

bNodeSocket &Int::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_INT,
                                                     this->subtype,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueInt &value = *(bNodeSocketValueInt *)socket.default_value;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.value = this->default_value;
  return socket;
}

bool Int::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_INT) {
    return false;
  }
  if (socket.typeinfo->subtype != this->subtype) {
    return false;
  }
  bNodeSocketValueInt &value = *(bNodeSocketValueInt *)socket.default_value;
  if (value.min != this->soft_min_value) {
    return false;
  }
  if (value.max != this->soft_max_value) {
    return false;
  }
  return true;
}

bool Int::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  return basic_types_can_connect(*this, socket);
}

bNodeSocket &Int::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_INT) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (socket.typeinfo->subtype != this->subtype) {
    modify_subtype_except_for_storage(socket, this->subtype);
  }
  this->set_common_flags(socket);
  bNodeSocketValueInt &value = *(bNodeSocketValueInt *)socket.default_value;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  value.subtype = this->subtype;
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Vector
 * \{ */

bNodeSocket &Vector::build(bNodeTree &ntree, bNode &node) const
{
  const StringRefNull idname = *bke::node_static_socket_type(
      SOCK_VECTOR, this->subtype, this->dimensions);
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, idname, this->identifier.c_str(), this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueVector &value = *(bNodeSocketValueVector *)socket.default_value;
  std::copy_n(&this->default_value[0], this->dimensions, value.value);
  value.dimensions = this->dimensions;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

bool Vector::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_VECTOR) {
    return false;
  }
  if (socket.typeinfo->subtype != this->subtype) {
    return false;
  }
  const bNodeSocketValueVector &value = *static_cast<const bNodeSocketValueVector *>(
      socket.default_value);
  if (value.dimensions != this->dimensions) {
    return false;
  }
  if (value.min != this->soft_min_value) {
    return false;
  }
  if (value.max != this->soft_max_value) {
    return false;
  }
  return true;
}

bool Vector::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  if (socket.type == SOCK_ROTATION) {
    return true;
  }
  return basic_types_can_connect(*this, socket);
}

bNodeSocket &Vector::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_VECTOR) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (socket.typeinfo->subtype != this->subtype) {
    modify_subtype_except_for_storage(socket, this->subtype, this->dimensions);
  }
  this->set_common_flags(socket);
  bNodeSocketValueVector &value = *(bNodeSocketValueVector *)socket.default_value;
  if (value.dimensions != this->dimensions) {
    modify_subtype_except_for_storage(socket, this->subtype, this->dimensions);
  }
  value.subtype = this->subtype;
  value.dimensions = this->dimensions;
  value.min = this->soft_min_value;
  value.max = this->soft_max_value;
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Bool
 * \{ */

bNodeSocket &Bool::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_BOOLEAN,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueBoolean &value = *(bNodeSocketValueBoolean *)socket.default_value;
  value.value = this->default_value;
  return socket;
}

bool Bool::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_BOOLEAN) {
    return false;
  }
  return true;
}

bool Bool::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  return basic_types_can_connect(*this, socket);
}

bNodeSocket &Bool::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_BOOLEAN) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Color
 * \{ */

bNodeSocket &Color::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_RGBA,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueRGBA &value = *(bNodeSocketValueRGBA *)socket.default_value;
  copy_v4_v4(value.value, this->default_value);
  return socket;
}

bool Color::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_RGBA) {
    return false;
  }
  return true;
}

bool Color::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  return basic_types_can_connect(*this, socket);
}

bNodeSocket &Color::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_RGBA) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name #Rotation
 * \{ */

bNodeSocket &Rotation::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_ROTATION,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  bNodeSocketValueRotation &value = *static_cast<bNodeSocketValueRotation *>(socket.default_value);
  copy_v3_v3(value.value_euler, float3(this->default_value));
  return socket;
}

bool Rotation::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_ROTATION) {
    return false;
  }
  return true;
}

bool Rotation::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  if (this->in_out == SOCK_IN) {
    return ELEM(socket.type, SOCK_ROTATION, SOCK_FLOAT, SOCK_VECTOR, SOCK_MATRIX);
  }
  return ELEM(socket.type, SOCK_ROTATION, SOCK_VECTOR, SOCK_MATRIX);
}

bNodeSocket &Rotation::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_ROTATION) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Matrix
 * \{ */

bNodeSocket &Matrix::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_MATRIX,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool Matrix::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_MATRIX) {
    return false;
  }
  return true;
}

bool Matrix::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  if (this->in_out == SOCK_IN) {
    return ELEM(socket.type, SOCK_MATRIX, SOCK_FLOAT, SOCK_VECTOR, SOCK_MATRIX);
  }
  return ELEM(socket.type, SOCK_MATRIX, SOCK_VECTOR, SOCK_MATRIX);
}

bNodeSocket &Matrix::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_MATRIX) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #String
 * \{ */

bNodeSocket &String::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_STRING,
                                                     this->subtype,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  STRNCPY(((bNodeSocketValueString *)socket.default_value)->value, this->default_value.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool String::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_STRING) {
    return false;
  }
  if (socket.typeinfo->subtype != this->subtype) {
    return false;
  }
  return true;
}

bool String::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_STRING;
}

bNodeSocket &String::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_STRING) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  if (socket.typeinfo->subtype != this->subtype) {
    modify_subtype_except_for_storage(socket, this->subtype);
  }
  this->set_common_flags(socket);
  bNodeSocketValueString &value = *(bNodeSocketValueString *)socket.default_value;
  value.subtype = this->subtype;
  return socket;
}

StringBuilder &StringBuilder::path_filter(std::optional<std::string> filter)
{
  BLI_assert(decl_->subtype == PROP_FILEPATH);
  decl_->path_filter = std::move(filter);
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Menu
 * \{ */

bNodeSocket &Menu::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_MENU,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());

  ((bNodeSocketValueMenu *)socket.default_value)->value = this->default_value;
  this->set_common_flags(socket);
  return socket;
}

bool Menu::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_MENU) {
    return false;
  }
  return true;
}

bool Menu::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_MENU;
}

bNodeSocket &Menu::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_MENU) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

MenuBuilder &MenuBuilder::static_items(const EnumPropertyItem *items)
{
  /* Using a global map ensures that the same runtime data is used for the same static items.
   * This is necessary because otherwise each node would have a different (incompatible) menu
   * definition. */
  static Mutex mutex;
  static Map<const EnumPropertyItem *, ImplicitSharingPtr<bke::RuntimeNodeEnumItems>>
      items_by_enum_ptr;

  std::lock_guard lock{mutex};
  decl_->items = items_by_enum_ptr.lookup_or_add_cb(items, [&]() {
    bke::RuntimeNodeEnumItems *runtime_items = new bke::RuntimeNodeEnumItems();
    for (const EnumPropertyItem *item = items; item->identifier; item++) {
      bke::RuntimeNodeEnumItem runtime_item;
      runtime_item.name = item->name;
      runtime_item.description = item->description;
      runtime_item.identifier = item->value;
      runtime_items->items.append(std::move(runtime_item));
    }
    return ImplicitSharingPtr<bke::RuntimeNodeEnumItems>(runtime_items);
  });
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Bundle
 * \{ */

bNodeSocket &Bundle::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_BUNDLE,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool Bundle::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_BUNDLE) {
    return false;
  }
  return true;
}

bool Bundle::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  return ELEM(socket.type, SOCK_BUNDLE);
}

bNodeSocket &Bundle::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_BUNDLE) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

BundleBuilder &BundleBuilder::pass_through_input_index(const std::optional<int> index)
{
  BLI_assert(this->is_output());
  decl_->pass_through_input_index = std::move(index);
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Closure
 * \{ */

bNodeSocket &Closure::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_static_socket(ntree,
                                                     node,
                                                     this->in_out,
                                                     SOCK_CLOSURE,
                                                     PROP_NONE,
                                                     this->identifier.c_str(),
                                                     this->name.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool Closure::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_CLOSURE) {
    return false;
  }
  return true;
}

bool Closure::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  return ELEM(socket.type, SOCK_CLOSURE);
}

bNodeSocket &Closure::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.type != SOCK_CLOSURE) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #IDSocketDeclaration
 * \{ */

bNodeSocket &IDSocketDeclaration::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, this->idname, this->identifier.c_str(), this->name.c_str());
  if (this->default_value_fn) {
    ID *id = this->default_value_fn(node);
    /* Assumes that all ID sockets like #bNodeSocketValueObject and #bNodeSocketValueImage have the
     * ID pointer at the start of the struct. */
    *static_cast<ID **>(socket.default_value) = id;
    id_us_plus(id);
  }
  this->set_common_flags(socket);
  return socket;
}

bool IDSocketDeclaration::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (!STREQ(socket.idname, this->idname)) {
    return false;
  }
  return true;
}

bool IDSocketDeclaration::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && STREQ(socket.idname, this->idname);
}

bNodeSocket &IDSocketDeclaration::update_or_build(bNodeTree &ntree,
                                                  bNode &node,
                                                  bNodeSocket &socket) const
{
  if (StringRef(socket.idname) != this->idname) {
    BLI_assert(socket.in_out == this->in_out);
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Geometry
 * \{ */

bNodeSocket &Geometry::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                              node,
                                              this->in_out,
                                              "NodeSocketGeometry",
                                              this->identifier.c_str(),
                                              this->name.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool Geometry::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_GEOMETRY) {
    return false;
  }
  return true;
}

bool Geometry::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && socket.type == SOCK_GEOMETRY;
}

Span<bke::GeometryComponent::Type> Geometry::supported_types() const
{
  return supported_types_;
}

bool Geometry::only_realized_data() const
{
  return only_realized_data_;
}

bool Geometry::only_instances() const
{
  return only_instances_;
}

GeometryBuilder &GeometryBuilder::supported_type(bke::GeometryComponent::Type supported_type)
{
  decl_->supported_types_ = {supported_type};
  return *this;
}

GeometryBuilder &GeometryBuilder::supported_type(
    blender::Vector<bke::GeometryComponent::Type> supported_types)
{
  decl_->supported_types_ = supported_types;
  return *this;
}

GeometryBuilder &GeometryBuilder::only_realized_data(bool value)
{
  decl_->only_realized_data_ = value;
  return *this;
}

GeometryBuilder &GeometryBuilder::only_instances(bool value)
{
  decl_->only_instances_ = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Shader
 * \{ */

bNodeSocket &Shader::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, "NodeSocketShader", this->identifier.c_str(), this->name.c_str());
  this->set_common_flags(socket);
  return socket;
}

bool Shader::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_SHADER) {
    return false;
  }
  return true;
}

bool Shader::can_connect(const bNodeSocket &socket) const
{
  if (!sockets_can_connect(*this, socket)) {
    return false;
  }
  /* Basic types can convert to shaders, but not the other way around. */
  if (this->in_out == SOCK_IN) {
    return ELEM(
        socket.type, SOCK_VECTOR, SOCK_RGBA, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_SHADER);
  }
  return socket.type == SOCK_SHADER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Extend
 * \{ */

bNodeSocket &Extend::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(ntree,
                                              node,
                                              this->in_out,
                                              "NodeSocketVirtual",
                                              this->identifier.c_str(),
                                              this->name.c_str());
  return socket;
}

bool Extend::matches(const bNodeSocket &socket) const
{
  if (socket.identifier != this->identifier) {
    return false;
  }
  return true;
}

bool Extend::can_connect(const bNodeSocket & /*socket*/) const
{
  return false;
}

bNodeSocket &Extend::update_or_build(bNodeTree & /*ntree*/,
                                     bNode & /*node*/,
                                     bNodeSocket &socket) const
{
  return socket;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Custom
 * \{ */

bNodeSocket &Custom::build(bNodeTree &ntree, bNode &node) const
{
  bNodeSocket &socket = *bke::node_add_socket(
      ntree, node, this->in_out, idname_, this->identifier.c_str(), this->name.c_str());
  if (this->init_socket_fn) {
    this->init_socket_fn(node, socket, "interface");
  }
  return socket;
}

bool Custom::matches(const bNodeSocket &socket) const
{
  if (!this->matches_common_data(socket)) {
    return false;
  }
  if (socket.type != SOCK_CUSTOM) {
    return false;
  }
  if (socket.typeinfo->idname != idname_) {
    return false;
  }
  return true;
}

bool Custom::can_connect(const bNodeSocket &socket) const
{
  return sockets_can_connect(*this, socket) && STREQ(socket.idname, idname_);
}

bNodeSocket &Custom::update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const
{
  if (socket.typeinfo->idname != idname_) {
    return this->build(ntree, node);
  }
  this->set_common_flags(socket);
  return socket;
}

/** \} */

}  // namespace blender::nodes::decl
