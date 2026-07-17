/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BLI_string_ref.hh"

#include "NOD_socket_declarations.hh"

namespace blender::nodes::logic::decl {

class Execution : public nodes::decl::Bool {
 public:
  using Builder = nodes::decl::BoolBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Condition : public nodes::decl::Bool {
 public:
  using Builder = nodes::decl::BoolBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Bool : public nodes::decl::Bool {
 public:
  using Builder = nodes::decl::BoolBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Int : public nodes::decl::Int {
 public:
  using Builder = nodes::decl::IntBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class CollisionLayers : public Int {
 public:
  using Builder = nodes::decl::IntBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
};

class Float : public nodes::decl::Float {
 public:
  using Builder = nodes::decl::FloatBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class String : public nodes::decl::String {
 public:
  using Builder = nodes::decl::StringBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Vector : public nodes::decl::Vector {
 public:
  using Builder = nodes::decl::VectorBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Rotation : public nodes::decl::Rotation {
 public:
  using Builder = nodes::decl::RotationBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Color : public nodes::decl::Color {
 public:
  using Builder = nodes::decl::ColorBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Object : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_OBJECT;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Object>;

  Object();
};

class Scene : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SCENE;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Scene>;

  Scene();
};

class Collection : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_COLLECTION;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Collection>;

  Collection();
};

class Material : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MATERIAL;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Material>;

  Material();
};

class Image : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_IMAGE;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Image>;

  Image();
};

class Sound : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SOUND;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Sound>;

  Sound();
};

class Font : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_FONT;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Font>;

  Font();
};

class Text : public nodes::decl::IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_TEXT_ID;

  using Builder = nodes::decl::IDSocketDeclarationBuilder<Text>;

  Text();
};

class GenericBuilder;
class ListBuilder;
class DictionaryBuilder;
class GeometryTreeBuilder;
class MeshBuilder;
class DatablockBuilder;
class UIBuilder;

class Generic : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = GenericBuilder;

  friend GenericBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class GenericBuilder : public SocketDeclarationBuilder<Generic> {};

class List : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = ListBuilder;

  friend ListBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class ListBuilder : public SocketDeclarationBuilder<List> {};

class Dictionary : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = DictionaryBuilder;

  friend DictionaryBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class DictionaryBuilder : public SocketDeclarationBuilder<Dictionary> {};

class GeometryTree : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = GeometryTreeBuilder;

  friend GeometryTreeBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class GeometryTreeBuilder : public SocketDeclarationBuilder<GeometryTree> {};

class Mesh : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = MeshBuilder;

  friend MeshBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class MeshBuilder : public SocketDeclarationBuilder<Mesh> {};

class Datablock : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = DatablockBuilder;

  friend DatablockBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class DatablockBuilder : public SocketDeclarationBuilder<Datablock> {};

class UI : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = UIBuilder;

  friend UIBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class UIBuilder : public SocketDeclarationBuilder<UI> {};

}  // namespace blender::nodes::logic::decl
