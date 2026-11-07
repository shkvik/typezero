#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class KeyedFieldChecker {
public:
  using ResultKeyedFieldRead = Result<Expr *, KeyedFieldCheckerCheckKeyedFieldError>;

  explicit KeyedFieldChecker(CheckState &owner);

  std::optional<std::string> constantKey(const Expr *key) const;
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  ResultKeyedFieldRead checkKeyedField(IndexExpr *index, bool write);
  static const Type *keyedFieldType(const Type *type, const Type *keyType);
  static bool wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code);

private:
  static bool isInherited(llvm::StringRef name);

  CheckState &state;
};

}
