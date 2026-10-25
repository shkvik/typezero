#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Intl beyond dates: options for the runtime and the bound format and compare.
class IntlLowerer {
public:
  explicit IntlLowerer(CodeGen &codegen);

  // The TzIntlOptions of a locale and options (either may be null) on the
  // stack; `held` gets the operands to drop after the call.
  llvm::Value *emitIntlOptions(Expr *locale, Expr *options, std::vector<CodeGen::Operand> &held);
  CodeGen::IntlNumber emitIntlNumber(Expr *arg);
  std::vector<llvm::Value *> intlNumberArgs(const CodeGen::IntlNumber &n);
  // The TzIntlShape of objects of `type` that the runtime makes.
  llvm::Constant *intlShape(const Type *type);
  // The info of `nf.format` and the like as a value of `fnType`.
  llvm::Constant *intlBoundInfo(IntlKind kind, const Type *fnType);
  // The locales of getCanonicalLocales and supportedLocalesOf, as an array.
  llvm::Value *emitLocaleArray(Expr *arg);

private:
  CodeGen &codegen_;
  llvm::DenseMap<const Type *, llvm::Constant *> intlShapes_;
  std::map<std::pair<IntlKind, const Type *>, llvm::Constant *> intlBound_;
};

} // namespace tz
