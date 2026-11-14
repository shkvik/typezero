#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SMLoc.h"
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace tz {

struct AliasInstance {
  const Type *type = nullptr;
  Type *self = nullptr;
  bool resolving = false;
  bool recursive = false;
};

struct AliasOrigin {
  TypeAliasDecl *alias = nullptr;
  std::vector<const Type *> args;
};

struct AliasParams {
  std::vector<const Type *> params;
  std::vector<const Type *> defaults;
};

class AliasContext {
public:
  void enterAlias();
  void leaveAlias();
  void rememberAliasUse(llvm::SMLoc loc);
  void rememberAliasParams(const TypeAliasDecl *alias, AliasParams params);
  void rememberAliasOrigin(const Type *type, TypeAliasDecl *alias, std::vector<const Type *> args);


  unsigned getAliasDepth() const;
  llvm::SMLoc getAliasUse() const;
  const AliasOrigin *findAliasOrigin(const Type *type) const;
  std::optional<AliasParams> findAliasParams(const TypeAliasDecl *alias) const;
  AliasInstance *recordAliasInstance(const TypeAliasDecl *alias, std::vector<const Type *> args);

private:
  llvm::SMLoc aliasUse;
  unsigned aliasDepth = 0;
  llvm::DenseMap<const Type *, AliasOrigin> aliasOrigins;
  llvm::DenseMap<const TypeAliasDecl *, AliasParams> aliasParams;
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, AliasInstance>
      aliasInstances;
};

}
