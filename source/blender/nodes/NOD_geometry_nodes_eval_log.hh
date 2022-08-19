/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Many geometry nodes related UI features need access to data produced during evaluation. Not only
 * is the final output required but also the intermediate results. Those features include
 * attribute search, node warnings, socket inspection and the viewer node.
 *
 * This file provides the framework for logging data during evaluation and accessing the data after
 * evaluation.
 *
 * During logging every thread gets its own local logger to avoid too much locking (logging
 * generally happens for every socket). After geometry nodes evaluation is done, the thread-local
 * logging information is combined and post-processed to make it easier for the UI to lookup.
 * necessary information.
 */

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_function_ref.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_map.hh"

#include "BKE_geometry_set.hh"

#include "NOD_derived_node_tree.hh"

#include "FN_field.hh"

#include <chrono>

struct SpaceNode;
struct SpaceSpreadsheet;

namespace blender::nodes::geometry_nodes_eval_log {

/** Contains information about a value that has been computed during geometry nodes evaluation. */
class ValueLog {
 public:
  virtual ~ValueLog() = default;
};

/** Contains an owned copy of a value of a generic type. */
class GenericValueLog : public ValueLog {
 private:
  GMutablePointer data_;

 public:
  GenericValueLog(GMutablePointer data) : data_(data)
  {
  }

  ~GenericValueLog()
  {
    data_.destruct();
  }

  GPointer value() const
  {
    return data_;
  }
};

class GFieldValueLog : public ValueLog {
 private:
  fn::GField field_;
  const CPPType &type_;
  Vector<std::string> input_tooltips_;

 public:
  GFieldValueLog(fn::GField field, bool log_full_field);

  const fn::GField &field() const
  {
    return field_;
  }

  Span<std::string> input_tooltips() const
  {
    return input_tooltips_;
  }

  const CPPType &type() const
  {
    return type_;
  }
};

struct GeometryAttributeInfo {
  std::string name;
  /** Can be empty when #name does not actually exist on a geometry yet. */
  std::optional<eAttrDomain> domain;
  std::optional<eCustomDataType> data_type;
};

/** Contains information about a geometry set. In most cases this does not store the entire
 * geometry set as this would require too much memory. */
class GeometryValueLog : public ValueLog {
 private:
  Vector<GeometryAttributeInfo> attributes_;
  Vector<GeometryComponentType> component_types_;
  std::unique_ptr<GeometrySet> full_geometry_;

 public:
  struct MeshInfo {
    int verts_num, edges_num, faces_num;
  };
  struct CurveInfo {
    int splines_num;
  };
  struct PointCloudInfo {
    int points_num;
  };
  struct InstancesInfo {
    int instances_num;
  };
  struct EditDataInfo {
    bool has_deformed_positions;
    bool has_deform_matrices;
  };

  std::optional<MeshInfo> mesh_info;
  std::optional<CurveInfo> curve_info;
  std::optional<PointCloudInfo> pointcloud_info;
  std::optional<InstancesInfo> instances_info;
  std::optional<EditDataInfo> edit_data_info;

  GeometryValueLog(const GeometrySet &geometry_set, bool log_full_geometry = false);

  Span<GeometryAttributeInfo> attributes() const
  {
    return attributes_;
  }

  Span<GeometryComponentType> component_types() const
  {
    return component_types_;
  }

  const GeometrySet *full_geometry() const
  {
    return full_geometry_.get();
  }
};

enum class NodeWarningType {
  Error,
  Warning,
  Info,
};

struct NodeWarning {
  NodeWarningType type;
  std::string message;
};

struct NodeWithWarning {
  DNode node;
  NodeWarning warning;
};

struct NodeWithExecutionTime {
  DNode node;
  std::chrono::microseconds exec_time;
};

struct NodeWithDebugMessage {
  DNode node;
  std::string message;
};

/** The same value can be referenced by multiple sockets when they are linked. */
struct ValueOfSockets {
  Span<DSocket> sockets;
  destruct_ptr<ValueLog> value;
};

enum class eNamedAttrUsage {
  None = 0,
  Read = 1 << 0,
  Write = 1 << 1,
  Remove = 1 << 2,
};
ENUM_OPERATORS(eNamedAttrUsage, eNamedAttrUsage::Remove);

struct UsedNamedAttribute {
  std::string name;
  eNamedAttrUsage usage;
};

struct NodeWithUsedNamedAttribute {
  DNode node;
  UsedNamedAttribute attribute;
};

class GeoLogger;
class ModifierLog;

/** Every thread has its own local logger to avoid having to communicate between threads during
 * evaluation. After evaluation the individual logs are combined. */
class LocalGeoLogger {
 private:
  /* Back pointer to the owner of this local logger. */
  GeoLogger *main_logger_;
  /* Allocator for the many small allocations during logging. This is in a `unique_ptr` so that
   * ownership can be transferred later on. */
  std::unique_ptr<LinearAllocator<>> allocator_;
  Vector<ValueOfSockets> values_;
  Vector<NodeWithWarning> node_warnings_;
  Vector<NodeWithExecutionTime> node_exec_times_;
  Vector<NodeWithDebugMessage> node_debug_messages_;
  Vector<NodeWithUsedNamedAttribute> used_named_attributes_;

  friend ModifierLog;

 public:
  LocalGeoLogger(GeoLogger &main_logger) : main_logger_(&main_logger)
  {
    this->allocator_ = std::make_unique<LinearAllocator<>>();
  }

  void log_value_for_sockets(Span<DSocket> sockets, GPointer value);
  void log_multi_value_socket(DSocket socket, Span<GPointer> values);
  void log_node_warning(DNode node, NodeWarningType type, std::string message);
  void log_execution_time(DNode node, std::chrono::microseconds exec_time);
  void log_used_named_attribute(DNode node, std::string attribute_name, eNamedAttrUsage usage);
  /**
   * Log a message that will be displayed in the node editor next to the node.
   * This should only be used for debugging purposes and not to display information to users.
   */
  void log_debug_message(DNode node, std::string message);
};

/** The root logger class. */
class GeoLogger {
 private:
  /**
   * Log the entire value for these sockets, because they may be inspected afterwards.
   * We don't log everything, because that would take up too much memory and cause significant
   * slowdowns.
   */
  Set<DSocket> log_full_sockets_;
  threading::EnumerableThreadSpecific<LocalGeoLogger> threadlocals_;

  /* These are only optional since they don't have a default constructor. */
  std::unique_ptr<GeometryValueLog> input_geometry_log_;
  std::unique_ptr<GeometryValueLog> output_geometry_log_;

  friend LocalGeoLogger;
  friend ModifierLog;

 public:
  GeoLogger(Set<DSocket> log_full_sockets)
      : log_full_sockets_(std::move(log_full_sockets)),
        threadlocals_([this]() { return LocalGeoLogger(*this); })
  {
  }

  void log_input_geometry(const GeometrySet &geometry)
  {
    input_geometry_log_ = std::make_unique<GeometryValueLog>(geometry);
  }

  void log_output_geometry(const GeometrySet &geometry)
  {
    output_geometry_log_ = std::make_unique<GeometryValueLog>(geometry);
  }

  LocalGeoLogger &local()
  {
    return threadlocals_.local();
  }

  auto begin()
  {
    return threadlocals_.begin();
  }

  auto end()
  {
    return threadlocals_.end();
  }
};

/** Contains information that has been logged for one specific socket. */
class SocketLog {
 private:
  ValueLog *value_ = nullptr;

  friend ModifierLog;

 public:
  const ValueLog *value() const
  {
    return value_;
  }
};

/** Contains information that has been logged for one specific node. */
class NodeLog {
 private:
  Vector<SocketLog> input_logs_;
  Vector<SocketLog> output_logs_;
  Vector<NodeWarning, 0> warnings_;
  Vector<std::string, 0> debug_messages_;
  Vector<UsedNamedAttribute, 0> used_named_attributes_;
  std::chrono::microseconds exec_time_;

  friend ModifierLog;

 public:
  const SocketLog *lookup_socket_log(eNodeSocketInOut in_out, int index) const;
  const SocketLog *lookup_socket_log(const bNode &node, const bNodeSocket &socket) const;
  void execution_time(std::chrono::microseconds exec_time);

  Span<SocketLog> input_logs() const
  {
    return input_logs_;
  }

  Span<SocketLog> output_logs() const
  {
    return output_logs_;
  }

  Span<NodeWarning> warnings() const
  {
    return warnings_;
  }

  Span<std::string> debug_messages() const
  {
    return debug_messages_;
  }

  Span<UsedNamedAttribute> used_named_attributes() const
  {
    return used_named_attributes_;
  }

  std::chrono::microseconds execution_time() const
  {
    return exec_time_;
  }

  Vector<const GeometryAttributeInfo *> lookup_available_attributes() const;
};

/** Contains information that has been logged for one specific tree. */
class TreeLog {
 private:
  Map<std::string, destruct_ptr<NodeLog>> node_logs_;
  Map<std::string, destruct_ptr<TreeLog>> child_logs_;

  friend ModifierLog;

 public:
  const NodeLog *lookup_node_log(StringRef node_name) const;
  const NodeLog *lookup_node_log(const bNode &node) const;
  const TreeLog *lookup_child_log(StringRef node_name) const;
  void foreach_node_log(FunctionRef<void(const NodeLog &)> fn) const;
};

/** Contains information about an entire geometry nodes evaluation. */
class ModifierLog {
 private:
  LinearAllocator<> allocator_;
  /* Allocators of the individual loggers. */
  Vector<std::unique_ptr<LinearAllocator<>>> logger_allocators_;
  destruct_ptr<TreeLog> root_tree_logs_;
  Vector<destruct_ptr<ValueLog>> logged_values_;

  std::unique_ptr<GeometryValueLog> input_geometry_log_;
  std::unique_ptr<GeometryValueLog> output_geometry_log_;

 public:
  ModifierLog(GeoLogger &logger);

  const TreeLog &root_tree() const
  {
    return *root_tree_logs_;
  }

  /* Utilities to find logged information for a specific context. */
  static const ModifierLog *find_root_by_node_editor_context(const SpaceNode &snode);
  static const TreeLog *find_tree_by_node_editor_context(const SpaceNode &snode);
  static const NodeLog *find_node_by_node_editor_context(const SpaceNode &snode,
                                                         const bNode &node);
  static const NodeLog *find_node_by_node_editor_context(const SpaceNode &snode,
                                                         const StringRef node_name);
  static const SocketLog *find_socket_by_node_editor_context(const SpaceNode &snode,
                                                             const bNode &node,
                                                             const bNodeSocket &socket);
  static const NodeLog *find_node_by_spreadsheet_editor_context(
      const SpaceSpreadsheet &sspreadsheet);
  void foreach_node_log(FunctionRef<void(const NodeLog &)> fn) const;

  const GeometryValueLog *input_geometry_log() const;
  const GeometryValueLog *output_geometry_log() const;

 private:
  using LogByTreeContext = Map<const DTreeContext *, TreeLog *>;

  TreeLog &lookup_or_add_tree_log(LogByTreeContext &log_by_tree_context,
                                  const DTreeContext &tree_context);
  NodeLog &lookup_or_add_node_log(LogByTreeContext &log_by_tree_context, DNode node);
  SocketLog &lookup_or_add_socket_log(LogByTreeContext &log_by_tree_context, DSocket socket);
};

}  // namespace blender::nodes::geometry_nodes_eval_log
