#pragma once

#include "compiler/pipeline/check_program/check_state/flow_state/flow_state_types.h"
#include "compiler/pipeline/check_program/check_state/library_declarations/library_declarations.h"
#include "compiler/pipeline/check_program/scopes/scope_entry.h"
#include "compiler/program/ast/ast.h"
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
