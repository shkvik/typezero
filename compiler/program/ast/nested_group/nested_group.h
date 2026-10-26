#pragma once

#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

struct NestedGroup {
  std::vector<FunctionDecl *> members;
  std::vector<VarSymbol *> captures;
  FunctionDecl *owner = nullptr;
  size_t firstUse = ~size_t(0);
  unsigned creationSeq = 0;
  size_t scopeDepth = 0, functionDepth = 0;
  std::vector<llvm::StringRef> declaredNames, assignedNames;
  bool finished = false;
  struct Use {
    FunctionDecl *fn;
    unsigned seq;
    llvm::SMLoc loc;
  };
  std::vector<Use> uses;
};

}
