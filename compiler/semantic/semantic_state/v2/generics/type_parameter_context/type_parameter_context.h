#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

class TypeParameterContext {
public:
  void enterSubstitution(const Type *type);
  void leaveSubstitution(const Type *type);
  void rememberExactParams(const FuncSymbol *generic, std::vector<bool> exact);
  void rememberNumberParam(const Type *param, const Type *only, const Type *number);


  bool isSubstituting(const Type *type) const;


  const Type *findNumberParam(const Type *param, const Type *only) const;
  std::optional<std::vector<bool>> findExactParams(const FuncSymbol *generic) const;

private:
  llvm::SmallPtrSet<const Type *, 4> substituting;
  llvm::DenseMap<const FuncSymbol *, std::vector<bool>> exactParams;
  llvm::DenseMap<std::pair<const Type *, const Type *>, const Type *> numberParams;
};

}
