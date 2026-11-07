#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/types/conditional_types/conditional_types_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ConditionalTypes {
public:
  using ResultIntersectionJoined = Result<const Type *, ConditionalTypesResolveIntersectionError>;

  explicit ConditionalTypes(CheckState &owner);

  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  ResultIntersectionJoined resolveIntersection(const TypeRef &ref);
  const Type *resolveTypeArgument(const TypeRef &ref);
  const Type *signatureForUtility(const TypeRef &ref);

private:
  CheckState &state;
};

}
