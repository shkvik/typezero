#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Keys in brackets: o[k] of objects.
class KeyedFieldChecker {
public:
  explicit KeyedFieldChecker(Checker &checker);

  // Keys in brackets: the field a constant key names (a string literal, a
  // template without values, an enum member or a constant holding one of
  // those), or nothing.
  std::optional<std::string> constantKey(const Expr *key) const;
  // The fields `o[key]` may stand for, judged by the form of the unchecked
  // key alone (see SourceFile::stringConstants).
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  // `o[key]` of an object or a tuple, whose object is checked: the field it
  // names, as `o.name`, or an error. With `write`, as an assignment target.
  Expr *checkKeyedField(IndexExpr *index, bool write);
  // The type of the fields of `type` that a key of `keyType`, string literals
  // only, may name, when they are all of one type; null if it is no such key.
  static const Type *keyedFieldType(const Type *type, const Type *keyType);

private:
  Checker &checker_;
};

} // namespace tz
