#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SMLoc.h"
#include <utility>
#include <vector>

namespace tz {

using DestinationPlaces = std::vector<std::pair<const Type *, llvm::SMLoc>>;

class DestinationContext {
public:
  void rememberDestinations(const VarSymbol *variable, DestinationPlaces places);


  DestinationPlaces getDestinations(const VarSymbol *variable) const;

private:
  llvm::DenseMap<const VarSymbol *, DestinationPlaces> destinations;
};

}
