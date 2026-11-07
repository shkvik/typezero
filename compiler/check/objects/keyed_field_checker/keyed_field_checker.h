#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class KeyedFieldChecker {
public:
  using ResultKeyedFieldRead = Result<Expr *, KeyedFieldCheckerCheckKeyedFieldError>;

  explicit KeyedFieldChecker(Checker &owner);

  std::optional<std::string> constantKey(const Expr *key) const;
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  ResultKeyedFieldRead checkKeyedField(IndexExpr *index, bool write);
  static const Type *keyedFieldType(const Type *type, const Type *keyType);
  static bool wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code);

private:
  static bool isInherited(llvm::StringRef name);
  static std::string toUtf8(const std::u16string &text);

  Checker &checker;
};

}
