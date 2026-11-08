#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_operators/type_operators_errors.h"

namespace tz {

class TypeOperators {
public:
  using ResultTypeOpResolved = Result<const Type *, TypeOperatorsResolveTypeOpError>;

  explicit TypeOperators(SemanticState &owner);

  const Type *keysOf(const Type *type);
  ResultTypeOpResolved resolveTypeOp(const TypeRef &ref);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);

private:
  static std::u16string utf16(llvm::StringRef text);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  SemanticState &state;
};

}
