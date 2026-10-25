#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Intl beyond dates: options for the runtime and the bound format and compare.
class IntlLowerer {
public:
  explicit IntlLowerer(Lowering &lowering);

  // The TzIntlOptions of a locale and options (either may be null) on the
  // stack; `held` gets the operands to drop after the call.
  llvm::Value *emitIntlOptions(Expr *locale, Expr *options, std::vector<Lowering::Operand> &held);
  Lowering::IntlNumber emitIntlNumber(Expr *arg);
  std::vector<llvm::Value *> intlNumberArgs(const Lowering::IntlNumber &n);
  // The TzIntlShape of objects of `type` that the runtime makes.
  llvm::Constant *intlShape(const Type *type);
  // The info of `nf.format` and the like as a value of `fnType`.
  llvm::Constant *intlBoundInfo(IntlKind kind, const Type *fnType);
  // The locales of getCanonicalLocales and supportedLocalesOf, as an array.
  llvm::Value *emitLocaleArray(Expr *arg);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::Constant *> intlShapes_;
  std::map<std::pair<IntlKind, const Type *>, llvm::Constant *> intlBound_;
};

} // namespace tz
