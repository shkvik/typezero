#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class KeyedFieldChecker {
public:
  using ResultKeyedFieldRead = Result<Expr *, KeyedFieldCheckerCheckKeyedFieldError>;

  explicit KeyedFieldChecker(SemanticState &owner);

  std::optional<std::string> constantKey(const Expr *key) const;
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  ResultKeyedFieldRead checkKeyedField(IndexExpr *index, bool write);
  static const Type *keyedFieldType(const Type *type, const Type *keyType);
  static bool wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code);

private:
  static bool isInherited(llvm::StringRef name);

  SemanticState &state;
};

}
