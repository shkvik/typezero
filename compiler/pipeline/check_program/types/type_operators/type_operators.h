#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/types/type_operators/type_operators_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TypeOperators {
public:
  using ResultTypeOpResolved = Result<const Type *, TypeOperatorsResolveTypeOpError>;

  explicit TypeOperators(CheckState &owner);

  const Type *keysOf(const Type *type);
  ResultTypeOpResolved resolveTypeOp(const TypeRef &ref);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);

private:
  static std::u16string utf16(llvm::StringRef text);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  CheckState &state;
};

}
