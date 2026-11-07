#pragma once

#include "compiler/check/checker/checker.h"
#include <set>
#include <string>
#include <utility>

namespace tz {

class BuiltinModuleTypes {
public:
  explicit BuiltinModuleTypes(Checker &owner);

  void declare(llvm::ArrayRef<llvm::StringLiteral> pieces, llvm::StringRef bufferName);
  void exportFrom(NodeModule module, llvm::StringRef name);
  bool declared(llvm::StringRef name) const;
  TypeAliasDecl *alias(llvm::StringRef name) const;
  TypeAliasDecl *exported(NodeModule module, llvm::StringRef name) const;
  const Type *named(llvm::StringRef name);

private:
  Checker &checker;
  llvm::StringMap<TypeAliasDecl *> aliases;
  std::set<std::pair<NodeModule, std::string>> exports;
};

}
