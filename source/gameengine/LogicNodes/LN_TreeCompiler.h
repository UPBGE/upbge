/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LN_NodeRegistry.h"
#include "LN_Program.h"
#include "LN_Types.h"

namespace blender {
struct bNode;
struct bNodeTree;
}  // namespace blender

class LN_TreeCompiler {
 public:
  enum class CompileHandlerKind : uint8_t {
    Unsupported = 0,
    EventSource,
    ValueExpression,
    Expression,
    Command,
  };

  struct CompileHandlerInfo {
    CompileHandlerKind kind = CompileHandlerKind::Unsupported;
    bool emits_commands = false;
    bool requires_flow_input = false;
    bool has_runtime_implementation = false;
  };

  struct CompileHandlerDescriptor {
    LN_NodeKind node_kind = LN_NodeKind::ValueBool;
    CompileHandlerInfo info;
    const char *handler_id = nullptr;
  };

  explicit LN_TreeCompiler(const LN_NodeRegistry &registry = LN_NodeRegistry::GetBuiltin());

  static CompileHandlerInfo GetCompileHandlerInfo(LN_NodeKind kind);
  static const CompileHandlerDescriptor *FindCompileHandlerDescriptor(LN_NodeKind kind);
  static const CompileHandlerDescriptor *GetCompileHandlerDescriptors(size_t &r_count);
  static bool ValidateCompileHandlerInventory(std::vector<std::string> *r_errors = nullptr);
  static bool ValidateCompileHandlerDescriptors(const CompileHandlerDescriptor *descriptors,
                                                size_t count,
                                                std::vector<std::string> *r_errors = nullptr);

  /** How a registered node kind is compiled during the topology pass. */
  enum class CompileDispatch : uint8_t {
    Unsupported = 0,
    EventSource,
    ConstantOutput,
    CustomCompile,
  };

  static CompileDispatch GetCompileDispatch(LN_NodeKind kind);
  static std::string BuildSourceChecksum(const blender::bNodeTree &tree);

  std::shared_ptr<LN_Program> Compile(const blender::bNodeTree &tree) const;

 private:
  const LN_NodeRegistry &m_registry;
};
