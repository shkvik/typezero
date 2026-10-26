#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GeneratorChecker {
public:
  explicit GeneratorChecker(Checker &checker);

  const Type *resolveIteratorType(const TypeRef &ref);
  const Type *generatorReturnType(const FunctionDecl *fn, const Type *declared);
  const Type *inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff, bool report);
  Expr *checkYield(YieldExpr *yield);
  static const Type *iteratedType(const Type *type);
  bool checkSendsNothing(const Type *iterator, llvm::SMLoc loc, llvm::StringRef what);
  Expr *destructuredIterator(Expr *init, const VarDeclarator &d);
  const Type *asyncIteratedType(const Type *type);

private:
  Checker &checker_;
};

}
