#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class KeyedFieldChecker {
public:
  explicit KeyedFieldChecker(Checker &checker);

  std::optional<std::string> constantKey(const Expr *key) const;
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  Expr *checkKeyedField(IndexExpr *index, bool write);
  static const Type *keyedFieldType(const Type *type, const Type *keyType);

private:
  Checker &checker_;
};

}
