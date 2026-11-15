#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/semantic/semantic_state/walk/flow_state_context/flow_state_types.h"
#include "compiler/semantic/semantic_state/walk/scope_stack_context/scope_entry.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SMLoc.h"
#include <optional>

namespace tz {

struct NodeName {
  NodeModule module;
  llvm::StringRef name;
};

struct ModuleBinding {
  Module *module = nullptr;
  llvm::StringRef name;
};

bool operator==(const ModuleBinding &left, const ModuleBinding &right);

struct ModuleExport {
  ModuleBinding target;
  llvm::SMLoc loc;
  const ExportAll *all = nullptr;
};

struct ModuleImport {
  ModuleBinding target;
  llvm::SMLoc loc;
};

struct ModuleState {
  llvm::StringMap<ScopeEntry> values;
  llvm::StringMap<Stmt *> types;
  llvm::StringMap<llvm::SMLoc> declared;
  llvm::StringSet<> topLevelNames;
  llvm::StringMap<ModuleImport> imports;
  llvm::StringSet<> badImports;
  llvm::SmallPtrSet<const VarSymbol *, 4> importedVars;
  llvm::StringMap<NodeName> nodeNames;
  llvm::StringMap<ModuleExport> exports;
  bool exportsDone = false, exportsBusy = false;
  size_t next = 0;
  std::optional<Flow> flow;
  bool busy = false;
  bool loaded = false;
};

}
