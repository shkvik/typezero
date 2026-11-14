#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseMap.h"
#include <cstdint>
#include <optional>

namespace tz {

class FixedLengthContext {
public:
  void rememberFixedLength(const VarSymbol *variable, int64_t length);


  std::optional<int64_t> findFixedLength(const VarSymbol *variable) const;

private:
  llvm::DenseMap<const VarSymbol *, int64_t> fixedLengths;
};

}
