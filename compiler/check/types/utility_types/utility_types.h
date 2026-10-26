#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UtilityTypes {
public:
  explicit UtilityTypes(Checker &checker);

  const Type *getterType(ClassDecl *cls, llvm::StringRef name);
  const Type *importedValueType(Module *from, llvm::StringRef name);
  const Type *resolveUtilityType(const TypeRef &ref);
  bool wantsLiteral(const Type *param) const;
  Expr *checkGenericIndex(IndexExpr *index);

private:
  Checker &checker_;
};

}
