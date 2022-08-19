/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup nodes
 *
 * DerivedNodeTree builds on top of NodeTreeRef and makes working with (nested) node groups more
 * convenient and safe. It does so by pairing nodes and sockets with a context. The context
 * contains information about the current "instance" of the node or socket. A node might be
 * "instanced" multiple times when it is in a node group that is used multiple times.
 */

#include "BLI_function_ref.hh"
#include "BLI_vector_set.hh"

#include "NOD_node_tree_ref.hh"

namespace blender::nodes {

class DTreeContext;
class DerivedNodeTree;

class DNode;
class DSocket;
class DInputSocket;
class DOutputSocket;

/**
 * The context attached to every node or socket in a derived node tree. It can be used to determine
 * the place of a node in a hierarchy of node groups.
 *
 * Contexts are organized in a tree data structure to avoid having to store the entire path to the
 * root node group for every node/socket.
 */
class DTreeContext {
 private:
  /* Null when this context is for the root node group. Otherwise it points to the context one
   * level up. */
  DTreeContext *parent_context_;
  /* Null when this context is for the root node group. Otherwise it points to the group node in
   * the parent node group that contains this context. */
  const NodeRef *parent_node_;
  /* The current node tree. */
  const NodeTreeRef *tree_;
  /* All the children contexts of this context. */
  Map<const NodeRef *, DTreeContext *> children_;
  DerivedNodeTree *derived_tree_;

  friend DerivedNodeTree;

 public:
  const NodeTreeRef &tree() const;
  const DTreeContext *parent_context() const;
  const NodeRef *parent_node() const;
  const DTreeContext *child_context(const NodeRef &node) const;
  const DerivedNodeTree &derived_tree() const;
  bool is_root() const;
};

/**
 * A (nullable) reference to a node and the context it is in. It is unique within an entire nested
 * node group hierarchy. This type is small and can be passed around by value.
 */
class DNode {
 private:
  const DTreeContext *context_ = nullptr;
  const NodeRef *node_ref_ = nullptr;

 public:
  DNode() = default;
  DNode(const DTreeContext *context, const NodeRef *node);

  const DTreeContext *context() const;
  const NodeRef *node_ref() const;
  const NodeRef *operator->() const;

  friend bool operator==(const DNode &a, const DNode &b);
  friend bool operator!=(const DNode &a, const DNode &b);
  operator bool() const;

  uint64_t hash() const;

  DInputSocket input(int index) const;
  DOutputSocket output(int index) const;

  DInputSocket input_by_identifier(StringRef identifier) const;
  DOutputSocket output_by_identifier(StringRef identifier) const;
};

/**
 * A (nullable) reference to a socket and the context it is in. It is unique within an entire
 * nested node group hierarchy. This type is small and can be passed around by value.
 *
 * A #DSocket can represent an input or an output socket. If the type of a socket is known at
 * compile time is preferable to use #DInputSocket or #DOutputSocket instead.
 */
class DSocket {
 protected:
  const DTreeContext *context_ = nullptr;
  const SocketRef *socket_ref_ = nullptr;

 public:
  DSocket() = default;
  DSocket(const DTreeContext *context, const SocketRef *socket);
  DSocket(const DInputSocket &input_socket);
  DSocket(const DOutputSocket &output_socket);

  const DTreeContext *context() const;
  const SocketRef *socket_ref() const;
  const SocketRef *operator->() const;

  friend bool operator==(const DSocket &a, const DSocket &b);
  friend bool operator!=(const DSocket &a, const DSocket &b);
  operator bool() const;

  uint64_t hash() const;

  DNode node() const;
};

/** A (nullable) reference to an input socket and the context it is in. */
class DInputSocket : public DSocket {
 public:
  DInputSocket() = default;
  DInputSocket(const DTreeContext *context, const InputSocketRef *socket);
  explicit DInputSocket(const DSocket &base_socket);

  const InputSocketRef *socket_ref() const;
  const InputSocketRef *operator->() const;

  DOutputSocket get_corresponding_group_node_output() const;
  Vector<DOutputSocket, 4> get_corresponding_group_input_sockets() const;

  /**
   * Call `origin_fn` for every "real" origin socket. "Real" means that reroutes, muted nodes
   * and node groups are handled by this function. Origin sockets are ones where a node gets its
   * inputs from.
   */
  void foreach_origin_socket(FunctionRef<void(DSocket)> origin_fn) const;
};

/** A (nullable) reference to an output socket and the context it is in. */
class DOutputSocket : public DSocket {
 public:
  DOutputSocket() = default;
  DOutputSocket(const DTreeContext *context, const OutputSocketRef *socket);
  explicit DOutputSocket(const DSocket &base_socket);

  const OutputSocketRef *socket_ref() const;
  const OutputSocketRef *operator->() const;

  DInputSocket get_corresponding_group_node_input() const;
  DInputSocket get_active_corresponding_group_output_socket() const;

  struct TargetSocketPathInfo {
    /** All sockets on the path from the current to the final target sockets, excluding `this`. */
    Vector<DSocket, 16> sockets;
  };

  using ForeachTargetSocketFn =
      FunctionRef<void(DInputSocket, const TargetSocketPathInfo &path_info)>;

  /**
   * Calls `target_fn` for every "real" target socket. "Real" means that reroutes, muted nodes
   * and node groups are handled by this function. Target sockets are on the nodes that use the
   * value from this socket.
   */
  void foreach_target_socket(ForeachTargetSocketFn target_fn) const;

 private:
  void foreach_target_socket(ForeachTargetSocketFn target_fn,
                             TargetSocketPathInfo &path_info) const;
};

class DerivedNodeTree {
 private:
  LinearAllocator<> allocator_;
  DTreeContext *root_context_;
  VectorSet<const NodeTreeRef *> used_node_tree_refs_;

 public:
  /**
   * Construct a new derived node tree for a given root node tree. The generated derived node tree
   * does not own the used node tree refs (so that those can be used by others as well). The caller
   * has to make sure that the node tree refs added to #node_tree_refs live at least as long as the
   * derived node tree.
   */
  DerivedNodeTree(bNodeTree &btree, NodeTreeRefMap &node_tree_refs);
  ~DerivedNodeTree();

  const DTreeContext &root_context() const;
  Span<const NodeTreeRef *> used_node_tree_refs() const;

  /**
   * \return True when there is a link cycle. Unavailable sockets are ignored.
   */
  bool has_link_cycles() const;
  bool has_undefined_nodes_or_sockets() const;
  /** Calls the given callback on all nodes in the (possibly nested) derived node tree. */
  void foreach_node(FunctionRef<void(DNode)> callback) const;

  /** Generates a graph in dot format. The generated graph has all node groups inlined. */
  std::string to_dot() const;

 private:
  DTreeContext &construct_context_recursively(DTreeContext *parent_context,
                                              const NodeRef *parent_node,
                                              bNodeTree &btree,
                                              NodeTreeRefMap &node_tree_refs);
  void destruct_context_recursively(DTreeContext *context);

  void foreach_node_in_context_recursive(const DTreeContext &context,
                                         FunctionRef<void(DNode)> callback) const;
};

namespace derived_node_tree_types {
using namespace node_tree_ref_types;
using nodes::DerivedNodeTree;
using nodes::DInputSocket;
using nodes::DNode;
using nodes::DOutputSocket;
using nodes::DSocket;
using nodes::DTreeContext;
}  // namespace derived_node_tree_types

/* -------------------------------------------------------------------- */
/** \name #DTreeContext Inline Methods
 * \{ */

inline const NodeTreeRef &DTreeContext::tree() const
{
  return *tree_;
}

inline const DTreeContext *DTreeContext::parent_context() const
{
  return parent_context_;
}

inline const NodeRef *DTreeContext::parent_node() const
{
  return parent_node_;
}

inline const DTreeContext *DTreeContext::child_context(const NodeRef &node) const
{
  return children_.lookup_default(&node, nullptr);
}

inline const DerivedNodeTree &DTreeContext::derived_tree() const
{
  return *derived_tree_;
}

inline bool DTreeContext::is_root() const
{
  return parent_context_ == nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #DNode Inline Methods
 * \{ */

inline DNode::DNode(const DTreeContext *context, const NodeRef *node_ref)
    : context_(context), node_ref_(node_ref)
{
  BLI_assert(node_ref == nullptr || &node_ref->tree() == &context->tree());
}

inline const DTreeContext *DNode::context() const
{
  return context_;
}

inline const NodeRef *DNode::node_ref() const
{
  return node_ref_;
}

inline bool operator==(const DNode &a, const DNode &b)
{
  return a.context_ == b.context_ && a.node_ref_ == b.node_ref_;
}

inline bool operator!=(const DNode &a, const DNode &b)
{
  return !(a == b);
}

inline DNode::operator bool() const
{
  return node_ref_ != nullptr;
}

inline const NodeRef *DNode::operator->() const
{
  return node_ref_;
}

inline uint64_t DNode::hash() const
{
  return get_default_hash_2(context_, node_ref_);
}

inline DInputSocket DNode::input(int index) const
{
  return {context_, &node_ref_->input(index)};
}

inline DOutputSocket DNode::output(int index) const
{
  return {context_, &node_ref_->output(index)};
}

inline DInputSocket DNode::input_by_identifier(StringRef identifier) const
{
  return {context_, &node_ref_->input_by_identifier(identifier)};
}

inline DOutputSocket DNode::output_by_identifier(StringRef identifier) const
{
  return {context_, &node_ref_->output_by_identifier(identifier)};
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #DSocket Inline Methods
 * \{ */

inline DSocket::DSocket(const DTreeContext *context, const SocketRef *socket_ref)
    : context_(context), socket_ref_(socket_ref)
{
  BLI_assert(socket_ref == nullptr || &socket_ref->tree() == &context->tree());
}

inline DSocket::DSocket(const DInputSocket &input_socket)
    : DSocket(input_socket.context_, input_socket.socket_ref_)
{
}

inline DSocket::DSocket(const DOutputSocket &output_socket)
    : DSocket(output_socket.context_, output_socket.socket_ref_)
{
}

inline const DTreeContext *DSocket::context() const
{
  return context_;
}

inline const SocketRef *DSocket::socket_ref() const
{
  return socket_ref_;
}

inline bool operator==(const DSocket &a, const DSocket &b)
{
  return a.context_ == b.context_ && a.socket_ref_ == b.socket_ref_;
}

inline bool operator!=(const DSocket &a, const DSocket &b)
{
  return !(a == b);
}

inline DSocket::operator bool() const
{
  return socket_ref_ != nullptr;
}

inline const SocketRef *DSocket::operator->() const
{
  return socket_ref_;
}

inline uint64_t DSocket::hash() const
{
  return get_default_hash_2(context_, socket_ref_);
}

inline DNode DSocket::node() const
{
  BLI_assert(socket_ref_ != nullptr);
  return {context_, &socket_ref_->node()};
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #DInputSocket Inline Methods
 * \{ */

inline DInputSocket::DInputSocket(const DTreeContext *context, const InputSocketRef *socket_ref)
    : DSocket(context, socket_ref)
{
}

inline DInputSocket::DInputSocket(const DSocket &base_socket) : DSocket(base_socket)
{
  BLI_assert(base_socket->is_input());
}

inline const InputSocketRef *DInputSocket::socket_ref() const
{
  return (const InputSocketRef *)socket_ref_;
}

inline const InputSocketRef *DInputSocket::operator->() const
{
  return (const InputSocketRef *)socket_ref_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #DOutputSocket Inline Methods
 * \{ */

inline DOutputSocket::DOutputSocket(const DTreeContext *context, const OutputSocketRef *socket_ref)
    : DSocket(context, socket_ref)
{
}

inline DOutputSocket::DOutputSocket(const DSocket &base_socket) : DSocket(base_socket)
{
  BLI_assert(base_socket->is_output());
}

inline const OutputSocketRef *DOutputSocket::socket_ref() const
{
  return (const OutputSocketRef *)socket_ref_;
}

inline const OutputSocketRef *DOutputSocket::operator->() const
{
  return (const OutputSocketRef *)socket_ref_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #DerivedNodeTree Inline Methods
 * \{ */

inline const DTreeContext &DerivedNodeTree::root_context() const
{
  return *root_context_;
}

inline Span<const NodeTreeRef *> DerivedNodeTree::used_node_tree_refs() const
{
  return used_node_tree_refs_;
}

/** \} */

}  // namespace blender::nodes
