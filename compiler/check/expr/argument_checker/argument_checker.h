#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Arguments against the parameters they go to.
class ArgumentChecker {
public:
  explicit ArgumentChecker(Checker &checker);

  // `checked` marks the arguments checked already (a generic call infers
  // its type arguments from them first, chooseVariant checks them to choose
  // a variant): those are only converted.
  Expr *checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                       bool hasRest, llvm::SMLoc loc, const std::vector<bool> *checked = nullptr);
  // An argument converted the way a parameter of type `to` takes it.
  Expr *checkArg(Expr *arg, const Type *to);
  // Reports a wrong number of arguments; checks them loosely then.
  bool checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max);

private:
  Checker &checker_;
};

} // namespace tz
