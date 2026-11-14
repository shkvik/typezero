#pragma once

#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/iterators/code_iterators_registry_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class CodeIteration {
public:
  using ResultSendsNothing = Result<const Type *, CodeIteratorsRegistryCheckSendsNothingError>;

  const Type *iteratedType(const Type *type) const;
  const Type *asyncIteratedType(const Type *type) const;
  ResultSendsNothing checkSendsNothing(const Type *iterator, llvm::StringRef what) const;
};

}
