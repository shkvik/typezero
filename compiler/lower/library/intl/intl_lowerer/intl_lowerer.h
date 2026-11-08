#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class IntlLowerer {
public:
  explicit IntlLowerer(Lowering &owner);

  llvm::Value *emitIntlOptions(Expr *locale, Expr *options, std::vector<Lowering::Operand> &held);
  Lowering::IntlNumber emitIntlNumber(Expr *arg);
  std::vector<llvm::Value *> intlNumberArgs(const Lowering::IntlNumber &n);
  llvm::Constant *intlShape(const Type *type);
  llvm::Constant *intlBoundInfo(IntlKind kind, const Type *fnType);
  llvm::Value *emitLocaleArray(Expr *arg);

private:
  static int optionIndex(llvm::StringRef name);

  Lowering &lowering;
  llvm::DenseMap<const Type *, llvm::Constant *> intlShapes;
  std::map<std::pair<IntlKind, const Type *>, llvm::Constant *> intlBound;
};

}
