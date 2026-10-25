#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Generic type aliases.
class AliasInstantiator {
public:
  explicit AliasInstantiator(Checker &checker);

  // `Pair<int>`: the type a generic alias names for these type arguments.
  // Those left out take their defaults.
  const Type *instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args,
                               llvm::SMLoc loc);
  // The type parameters of a generic alias, made when first needed.
  std::vector<const Type *> aliasTypeParams(TypeAliasDecl *alias);

private:
  Checker &checker_;
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, Checker::AliasInstance>
      aliasInstances_;
  llvm::DenseMap<const TypeAliasDecl *,
                 std::pair<std::vector<const Type *>, std::vector<const Type *>>>
      aliasParams_; // the type parameters and their defaults
  unsigned aliasDepth_ = 0;
  llvm::SMLoc aliasUse_; // see instantiateAlias
};

} // namespace tz
