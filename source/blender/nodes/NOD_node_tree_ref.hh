/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup nodes
 *
 * NodeTreeRef makes querying information about a bNodeTree more efficient. It is an immutable data
 * structure. It should not be used after anymore, after the underlying node tree changed.
 *
 * The following queries are supported efficiently:
 *  - socket -> index of socket
 *  - socket -> directly linked sockets
 *  - socket -> directly linked links
 *  - socket -> linked sockets when skipping reroutes
 *  - socket -> node
 *  - socket/node -> rna pointer
 *  - node -> inputs/outputs
 *  - node -> tree
 *  - tree -> all nodes
 *  - tree -> all (input/output) sockets
 *  - idname -> nodes
 *
 * Every socket has an id. The id-space is shared between input and output sockets.
 * When storing data per socket, it is often better to use the id as index into an array, instead
 * of a hash table.
 *
 * Every node has an id as well. The same rule regarding hash tables applies.
 *
 * There is an utility to export this data structure as graph in dot format.
 */

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_map.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_timeit.hh"
#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

#include "BKE_node.h"
#include "BKE_node_runtime.hh"

#include "DNA_node_types.h"

#include "RNA_access.h"

namespace blender::nodes {

class SocketRef;
class InputSocketRef;
class OutputSocketRef;
class NodeRef;
class NodeTreeRef;
class LinkRef;
class InternalLinkRef;

using SocketIndexByIdentifierMap = Map<std::string, int>;

class SocketRef : NonCopyable, NonMovable {
 protected:
  NodeRef *node_;
  bNodeSocket *bsocket_;
  bool is_input_;
  int id_;
  int index_;
  Vector<LinkRef *> directly_linked_links_;

  /* These sockets are linked directly, i.e. with a single link in between. */
  MutableSpan<const SocketRef *> directly_linked_sockets_;
  /* These sockets are linked when reroutes, muted links and muted nodes have been taken into
   * account. */
  MutableSpan<const SocketRef *> logically_linked_sockets_;
  /* These are the sockets that have been skipped when searching for logically linked sockets.
   * That includes for example the input and output socket of an intermediate reroute node. */
  MutableSpan<const SocketRef *> logically_linked_skipped_sockets_;

  friend NodeTreeRef;

 public:
  Span<const SocketRef *> logically_linked_sockets() const;
  Span<const SocketRef *> logically_linked_skipped_sockets() const;
  Span<const SocketRef *> directly_linked_sockets() const;
  Span<const LinkRef *> directly_linked_links() const;

  bool is_directly_linked() const;
  bool is_logically_linked() const;

  const NodeRef &node() const;
  const NodeTreeRef &tree() const;

  int id() const;
  int index() const;

  bool is_input() const;
  bool is_output() const;

  const SocketRef &as_base() const;
  const InputSocketRef &as_input() const;
  const OutputSocketRef &as_output() const;

  PointerRNA rna() const;

  StringRefNull idname() const;
  StringRefNull name() const;
  StringRefNull identifier() const;
  bNodeSocketType *typeinfo() const;

  bNodeSocket *bsocket() const;
  bNode *bnode() const;
  bNodeTree *btree() const;

  bool is_available() const;
  bool is_undefined() const;

  void *default_value() const;
  template<typename T> T *default_value() const;
};

class InputSocketRef final : public SocketRef {
 public:
  friend NodeTreeRef;

  Span<const OutputSocketRef *> logically_linked_sockets() const;
  Span<const OutputSocketRef *> directly_linked_sockets() const;

  bool is_multi_input_socket() const;

 private:
  void foreach_logical_origin(FunctionRef<void(const OutputSocketRef &)> origin_fn,
                              FunctionRef<void(const SocketRef &)> skipped_fn,
                              bool only_follow_first_input_link,
                              Vector<const InputSocketRef *> &seen_sockets_stack) const;
};

class OutputSocketRef final : public SocketRef {
 public:
  friend NodeTreeRef;

  Span<const InputSocketRef *> logically_linked_sockets() const;
  Span<const InputSocketRef *> directly_linked_sockets() const;

 private:
  void foreach_logical_target(FunctionRef<void(const InputSocketRef &)> target_fn,
                              FunctionRef<void(const SocketRef &)> skipped_fn,
                              Vector<const OutputSocketRef *> &seen_sockets_stack) const;
};

class NodeRef : NonCopyable, NonMovable {
 private:
  NodeTreeRef *tree_;
  bNode *bnode_;
  int id_;
  Vector<InputSocketRef *> inputs_;
  Vector<OutputSocketRef *> outputs_;
  Vector<InternalLinkRef *> internal_links_;
  SocketIndexByIdentifierMap *input_index_by_identifier_;
  SocketIndexByIdentifierMap *output_index_by_identifier_;

  friend NodeTreeRef;

 public:
  const NodeTreeRef &tree() const;

  Span<const InputSocketRef *> inputs() const;
  Span<const OutputSocketRef *> outputs() const;
  Span<const InternalLinkRef *> internal_links() const;
  Span<const SocketRef *> sockets(eNodeSocketInOut in_out) const;

  const InputSocketRef &input(int index) const;
  const OutputSocketRef &output(int index) const;

  const InputSocketRef &input_by_identifier(StringRef identifier) const;
  const OutputSocketRef &output_by_identifier(StringRef identifier) const;

  bool any_input_is_directly_linked() const;
  bool any_output_is_directly_linked() const;
  bool any_socket_is_directly_linked(eNodeSocketInOut in_out) const;

  bNode *bnode() const;
  bNodeTree *btree() const;

  PointerRNA rna() const;
  StringRefNull idname() const;
  StringRefNull name() const;
  StringRefNull label() const;
  StringRefNull label_or_name() const;
  bNodeType *typeinfo() const;
  const NodeDeclaration *declaration() const;

  int id() const;

  bool is_reroute_node() const;
  bool is_group_node() const;
  bool is_group_input_node() const;
  bool is_group_output_node() const;
  bool is_muted() const;
  bool is_frame() const;
  bool is_undefined() const;

  void *storage() const;
  template<typename T> T *storage() const;
};

class LinkRef : NonCopyable, NonMovable {
 private:
  OutputSocketRef *from_;
  InputSocketRef *to_;
  bNodeLink *blink_;

  friend NodeTreeRef;

 public:
  const OutputSocketRef &from() const;
  const InputSocketRef &to() const;

  bNodeLink *blink() const;

  bool is_muted() const;
};

class InternalLinkRef : NonCopyable, NonMovable {
 private:
  InputSocketRef *from_;
  OutputSocketRef *to_;
  bNodeLink *blink_;

  friend NodeTreeRef;

 public:
  const InputSocketRef &from() const;
  const OutputSocketRef &to() const;

  bNodeLink *blink() const;
};

class NodeTreeRef : NonCopyable, NonMovable {
 private:
  LinearAllocator<> allocator_;
  bNodeTree *btree_;
  Vector<NodeRef *> nodes_by_id_;
  Vector<SocketRef *> sockets_by_id_;
  Vector<InputSocketRef *> input_sockets_;
  Vector<OutputSocketRef *> output_sockets_;
  Vector<LinkRef *> links_;
  MultiValueMap<const bNodeType *, NodeRef *> nodes_by_type_;
  Vector<std::unique_ptr<SocketIndexByIdentifierMap>> owned_identifier_maps_;
  const NodeRef *group_output_node_ = nullptr;

 public:
  NodeTreeRef(bNodeTree *btree);
  ~NodeTreeRef();

  Span<const NodeRef *> nodes() const;
  Span<const NodeRef *> nodes_by_type(StringRefNull idname) const;
  Span<const NodeRef *> nodes_by_type(const bNodeType *nodetype) const;

  Span<const SocketRef *> sockets() const;
  Span<const InputSocketRef *> input_sockets() const;
  Span<const OutputSocketRef *> output_sockets() const;

  Span<const LinkRef *> links() const;

  const NodeRef *find_node(const bNode &bnode) const;

  /**
   * This is the active group output node if there are multiple.
   */
  const NodeRef *group_output_node() const;

  /**
   * \return True when there is a link cycle. Unavailable sockets are ignored.
   */
  bool has_link_cycles() const;
  bool has_undefined_nodes_or_sockets() const;

  enum class ToposortDirection {
    LeftToRight,
    RightToLeft,
  };

  struct ToposortResult {
    Vector<const NodeRef *> sorted_nodes;
    /**
     * There can't be a correct topological sort of the nodes when there is a cycle. The nodes will
     * still be sorted to some degree. The caller has to decide whether it can handle non-perfect
     * sorts or not.
     */
    bool has_cycle = false;
  };

  /**
   * Sort nodes topologically from left to right or right to left.
   * In the future the result if this could be cached on #NodeTreeRef.
   */
  ToposortResult toposort(ToposortDirection direction) const;

  bNodeTree *btree() const;
  StringRefNull name() const;

  std::string to_dot() const;

 private:
  /* Utility functions used during construction. */
  InputSocketRef &find_input_socket(Map<bNode *, NodeRef *> &node_mapping,
                                    bNode *bnode,
                                    bNodeSocket *bsocket);
  OutputSocketRef &find_output_socket(Map<bNode *, NodeRef *> &node_mapping,
                                      bNode *bnode,
                                      bNodeSocket *bsocket);

  void create_linked_socket_caches();
  void create_socket_identifier_maps();
};

using NodeTreeRefMap = Map<bNodeTree *, std::unique_ptr<const NodeTreeRef>>;

const NodeTreeRef &get_tree_ref_from_map(NodeTreeRefMap &node_tree_refs, bNodeTree &btree);

namespace node_tree_ref_types {
using nodes::InputSocketRef;
using nodes::NodeRef;
using nodes::NodeTreeRef;
using nodes::NodeTreeRefMap;
using nodes::OutputSocketRef;
using nodes::SocketRef;
}  // namespace node_tree_ref_types

/* -------------------------------------------------------------------- */
/** \name #SocketRef Inline Methods
 * \{ */

inline Span<const SocketRef *> SocketRef::logically_linked_sockets() const
{
  return logically_linked_sockets_;
}

inline Span<const SocketRef *> SocketRef::logically_linked_skipped_sockets() const
{
  return logically_linked_skipped_sockets_;
}

inline Span<const SocketRef *> SocketRef::directly_linked_sockets() const
{
  return directly_linked_sockets_;
}

inline Span<const LinkRef *> SocketRef::directly_linked_links() const
{
  return directly_linked_links_;
}

inline bool SocketRef::is_directly_linked() const
{
  return directly_linked_sockets_.size() > 0;
}

inline bool SocketRef::is_logically_linked() const
{
  return logically_linked_sockets_.size() > 0;
}

inline const NodeRef &SocketRef::node() const
{
  return *node_;
}

inline const NodeTreeRef &SocketRef::tree() const
{
  return node_->tree();
}

inline int SocketRef::id() const
{
  return id_;
}

inline int SocketRef::index() const
{
  return index_;
}

inline bool SocketRef::is_input() const
{
  return is_input_;
}

inline bool SocketRef::is_output() const
{
  return !is_input_;
}

inline const SocketRef &SocketRef::as_base() const
{
  return *this;
}

inline const InputSocketRef &SocketRef::as_input() const
{
  BLI_assert(this->is_input());
  return static_cast<const InputSocketRef &>(*this);
}

inline const OutputSocketRef &SocketRef::as_output() const
{
  BLI_assert(this->is_output());
  return static_cast<const OutputSocketRef &>(*this);
}

inline StringRefNull SocketRef::idname() const
{
  return bsocket_->idname;
}

inline StringRefNull SocketRef::name() const
{
  return bsocket_->name;
}

inline StringRefNull SocketRef::identifier() const
{
  return bsocket_->identifier;
}

inline bNodeSocketType *SocketRef::typeinfo() const
{
  return bsocket_->typeinfo;
}

inline bNodeSocket *SocketRef::bsocket() const
{
  return bsocket_;
}

inline bNode *SocketRef::bnode() const
{
  return node_->bnode();
}

inline bNodeTree *SocketRef::btree() const
{
  return node_->btree();
}

inline bool SocketRef::is_available() const
{
  return (bsocket_->flag & SOCK_UNAVAIL) == 0;
}

inline bool SocketRef::is_undefined() const
{
  return bsocket_->typeinfo == &NodeSocketTypeUndefined;
}

inline void *SocketRef::default_value() const
{
  return bsocket_->default_value;
}

template<typename T> inline T *SocketRef::default_value() const
{
  return (T *)bsocket_->default_value;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #InputSocketRef Inline Methods
 * \{ */

inline Span<const OutputSocketRef *> InputSocketRef::logically_linked_sockets() const
{
  return logically_linked_sockets_.as_span().cast<const OutputSocketRef *>();
}

inline Span<const OutputSocketRef *> InputSocketRef::directly_linked_sockets() const
{
  return directly_linked_sockets_.cast<const OutputSocketRef *>();
}

inline bool InputSocketRef::is_multi_input_socket() const
{
  return bsocket_->flag & SOCK_MULTI_INPUT;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #OutputSocketRef Inline Methods
 * \{ */

inline Span<const InputSocketRef *> OutputSocketRef::logically_linked_sockets() const
{
  return logically_linked_sockets_.as_span().cast<const InputSocketRef *>();
}

inline Span<const InputSocketRef *> OutputSocketRef::directly_linked_sockets() const
{
  return directly_linked_sockets_.cast<const InputSocketRef *>();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #NodeRef Inline Methods
 * \{ */

inline const NodeTreeRef &NodeRef::tree() const
{
  return *tree_;
}

inline Span<const InputSocketRef *> NodeRef::inputs() const
{
  return inputs_;
}

inline Span<const OutputSocketRef *> NodeRef::outputs() const
{
  return outputs_;
}

inline Span<const SocketRef *> NodeRef::sockets(const eNodeSocketInOut in_out) const
{
  return in_out == SOCK_IN ? inputs_.as_span().cast<const SocketRef *>() :
                             outputs_.as_span().cast<const SocketRef *>();
}

inline Span<const InternalLinkRef *> NodeRef::internal_links() const
{
  return internal_links_;
}

inline const InputSocketRef &NodeRef::input(int index) const
{
  return *inputs_[index];
}

inline const OutputSocketRef &NodeRef::output(int index) const
{
  return *outputs_[index];
}

inline const InputSocketRef &NodeRef::input_by_identifier(StringRef identifier) const
{
  const int index = input_index_by_identifier_->lookup_as(identifier);
  return this->input(index);
}

inline const OutputSocketRef &NodeRef::output_by_identifier(StringRef identifier) const
{
  const int index = output_index_by_identifier_->lookup_as(identifier);
  return this->output(index);
}

inline bNode *NodeRef::bnode() const
{
  return bnode_;
}

inline bNodeTree *NodeRef::btree() const
{
  return tree_->btree();
}

inline StringRefNull NodeRef::idname() const
{
  return bnode_->idname;
}

inline StringRefNull NodeRef::name() const
{
  return bnode_->name;
}

inline StringRefNull NodeRef::label() const
{
  return bnode_->label;
}

inline StringRefNull NodeRef::label_or_name() const
{
  const StringRefNull label = this->label();
  if (!label.is_empty()) {
    return label;
  }
  return this->name();
}

inline bNodeType *NodeRef::typeinfo() const
{
  return bnode_->typeinfo;
}

/* Returns a pointer because not all nodes have declarations currently. */
inline const NodeDeclaration *NodeRef::declaration() const
{
  nodeDeclarationEnsure(this->tree().btree(), bnode_);
  return bnode_->runtime->declaration;
}

inline int NodeRef::id() const
{
  return id_;
}

inline bool NodeRef::is_reroute_node() const
{
  return bnode_->type == NODE_REROUTE;
}

inline bool NodeRef::is_group_node() const
{
  return bnode_->type == NODE_GROUP || bnode_->type == NODE_CUSTOM_GROUP;
}

inline bool NodeRef::is_group_input_node() const
{
  return bnode_->type == NODE_GROUP_INPUT;
}

inline bool NodeRef::is_group_output_node() const
{
  return bnode_->type == NODE_GROUP_OUTPUT;
}

inline bool NodeRef::is_frame() const
{
  return bnode_->type == NODE_FRAME;
}

inline bool NodeRef::is_undefined() const
{
  return bnode_->typeinfo == &NodeTypeUndefined;
}

inline bool NodeRef::is_muted() const
{
  return (bnode_->flag & NODE_MUTED) != 0;
}

inline void *NodeRef::storage() const
{
  return bnode_->storage;
}

template<typename T> inline T *NodeRef::storage() const
{
  return (T *)bnode_->storage;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #LinkRef Inline Methods
 * \{ */

inline const OutputSocketRef &LinkRef::from() const
{
  return *from_;
}

inline const InputSocketRef &LinkRef::to() const
{
  return *to_;
}

inline bNodeLink *LinkRef::blink() const
{
  return blink_;
}

inline bool LinkRef::is_muted() const
{
  return blink_->flag & NODE_LINK_MUTED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #InternalLinkRef Inline Methods
 * \{ */

inline const InputSocketRef &InternalLinkRef::from() const
{
  return *from_;
}

inline const OutputSocketRef &InternalLinkRef::to() const
{
  return *to_;
}

inline bNodeLink *InternalLinkRef::blink() const
{
  return blink_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #NodeTreeRef Inline Methods
 * \{ */

inline Span<const NodeRef *> NodeTreeRef::nodes() const
{
  return nodes_by_id_;
}

inline Span<const NodeRef *> NodeTreeRef::nodes_by_type(StringRefNull idname) const
{
  const bNodeType *nodetype = nodeTypeFind(idname.c_str());
  return this->nodes_by_type(nodetype);
}

inline Span<const NodeRef *> NodeTreeRef::nodes_by_type(const bNodeType *nodetype) const
{
  return nodes_by_type_.lookup(nodetype);
}

inline Span<const SocketRef *> NodeTreeRef::sockets() const
{
  return sockets_by_id_;
}

inline Span<const InputSocketRef *> NodeTreeRef::input_sockets() const
{
  return input_sockets_;
}

inline Span<const OutputSocketRef *> NodeTreeRef::output_sockets() const
{
  return output_sockets_;
}

inline Span<const LinkRef *> NodeTreeRef::links() const
{
  return links_;
}

inline const NodeRef *NodeTreeRef::group_output_node() const
{
  return group_output_node_;
}

inline bNodeTree *NodeTreeRef::btree() const
{
  return btree_;
}

inline StringRefNull NodeTreeRef::name() const
{
  return btree_->id.name + 2;
}

/** \} */

}  // namespace blender::nodes
