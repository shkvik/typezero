#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

class ArgumentListContext {
public:
  void rememberArgumentList(const VarSymbol *variable, std::vector<llvm::StringRef> parts);


  std::optional<std::vector<llvm::StringRef>> findArgumentList(const VarSymbol *variable) const;

private:
  llvm::DenseMap<const VarSymbol *, std::vector<llvm::StringRef>> argumentLists;
};

}
