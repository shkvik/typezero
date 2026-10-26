#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class AliasInstantiator {
public:
  explicit AliasInstantiator(Checker &checker);

  const Type *instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args,
                               llvm::SMLoc loc);
  std::vector<const Type *> aliasTypeParams(TypeAliasDecl *alias);

private:
  Checker &checker_;
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, Checker::AliasInstance>
      aliasInstances_;
  llvm::DenseMap<const TypeAliasDecl *,
                 std::pair<std::vector<const Type *>, std::vector<const Type *>>>
      aliasParams_;
  unsigned aliasDepth_ = 0;
  llvm::SMLoc aliasUse_;
};

}
