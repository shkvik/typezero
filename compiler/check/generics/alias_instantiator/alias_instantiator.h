#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class AliasInstantiator {
public:
  explicit AliasInstantiator(Checker &owner);

  const Type *instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args,
                               llvm::SMLoc loc);
  std::vector<const Type *> aliasTypeParams(TypeAliasDecl *alias);

private:
  static std::string typeList(llvm::ArrayRef<const Type *> types);

  Checker &checker;
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, Checker::AliasInstance>
      aliasInstances;
  llvm::DenseMap<const TypeAliasDecl *,
                 std::pair<std::vector<const Type *>, std::vector<const Type *>>>
      aliasParams;
  unsigned aliasDepth = 0;
  llvm::SMLoc aliasUse;
};

}
