#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeOperators {
public:
  explicit TypeOperators(Checker &owner);

  const Type *keysOf(const Type *type);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);
  const Type *resolveTypeOp(const TypeRef &ref);

private:
  static std::u16string utf16(llvm::StringRef text);
  static std::string utf8(const std::u16string &text);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  Checker &checker;
};

}
