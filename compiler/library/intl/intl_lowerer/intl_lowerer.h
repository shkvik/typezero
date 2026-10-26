#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class IntlLowerer {
public:
  explicit IntlLowerer(Lowering &lowering);

  llvm::Value *emitIntlOptions(Expr *locale, Expr *options, std::vector<Lowering::Operand> &held);
  Lowering::IntlNumber emitIntlNumber(Expr *arg);
  std::vector<llvm::Value *> intlNumberArgs(const Lowering::IntlNumber &n);
  llvm::Constant *intlShape(const Type *type);
  llvm::Constant *intlBoundInfo(IntlKind kind, const Type *fnType);
  llvm::Value *emitLocaleArray(Expr *arg);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::Constant *> intlShapes_;
  std::map<std::pair<IntlKind, const Type *>, llvm::Constant *> intlBound_;
};

}
