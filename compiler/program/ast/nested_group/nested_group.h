#pragma once

#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

// The functions declared in one block. They are created together, before the
// first statement of the block that mentions one of them, and share one
// environment: they call each other (and themselves) without capturing each
// other, so no reference cycle forms.
struct NestedGroup {
  std::vector<FunctionDecl *> members;
  std::vector<VarSymbol *> captures;
  FunctionDecl *owner = nullptr; // the function whose block declares them
  // The statement of the block before which they are created; none if
  // nothing mentions them.
  size_t firstUse = ~size_t(0);
  unsigned creationSeq = 0;
  // Used by Checker while it checks the block: where the block's scope and
  // function are, the names the block declares and the names the functions
  // assign to.
  size_t scopeDepth = 0, functionDepth = 0;
  std::vector<llvm::StringRef> declaredNames, assignedNames;
  bool finished = false;
  // Where the block's own code uses the functions, to find those that run
  // before a variable they use is declared.
  struct Use {
    FunctionDecl *fn;
    unsigned seq;
    llvm::SMLoc loc;
  };
  std::vector<Use> uses;
};

} // namespace tz
