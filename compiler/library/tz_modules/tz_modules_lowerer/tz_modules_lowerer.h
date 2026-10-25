#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Calls of the runtime from the built-in modules written in tz, their accessors.
class TzModulesLowerer {
public:
  explicit TzModulesLowerer(Lowering &lowering);

  // An intrinsic of a built-in module written in tz (FunctionDecl::external,
  // codegen_library.cpp): a call of the runtime's function of its name.
  void emitExternal(FunctionDecl *fn);
  // The keys of the getters of the classes of `chain`, the most derived
  // last, for the descriptor of a class of the library (TZ_FIELD_GETTER).
  void appendGetterKeys(llvm::ArrayRef<const ClassDecl *> chain,
                        std::vector<llvm::Constant *> &entries);
  // A function value of another result (CastExpr::adaptsResult,
  // codegen_library.cpp): a box with it is the environment of code that
  // calls it and converts what it gives.
  llvm::Value *emitResultAdapter(CastExpr *cast);

private:
  Lowering &lowering_;
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> resultAdapters_;
};

} // namespace tz
