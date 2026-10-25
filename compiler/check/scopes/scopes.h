#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The names a block sees: declaring, finding, and whether one is visible here.
class Scopes {
public:
  explicit Scopes(Sema &sema);

  void declare(llvm::StringRef name, llvm::SMLoc loc, Sema::ScopeEntry entry);
  Sema::ScopeEntry lookup(llvm::StringRef name) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);
  const Type *varType(VarSymbol *var);
  // Reports a use of a variable declared by an earlier case of a switch.
  void checkVisible(NameExpr *name, const VarSymbol *var);

private:
  Sema &sema_;
};

} // namespace tz
