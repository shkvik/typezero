#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
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

struct DescriptorOrigin {
  const Type *args = nullptr;
  const Type *ret = nullptr;
};

class AliasStore {
public:
  AliasInstance *recordAliasInstance(const TypeAliasDecl *alias, std::vector<const Type *> args);
  void rememberAliasOrigin(const Type *type, TypeAliasDecl *alias, std::vector<const Type *> args);
  const AliasOrigin *findAliasOrigin(const Type *type) const;
  void rememberAliasParams(const TypeAliasDecl *alias, AliasParams params);
  std::optional<AliasParams> findAliasParams(const TypeAliasDecl *alias) const;
  void enterAlias();
  void leaveAlias();
  unsigned getAliasDepth() const;
  void rememberAliasUse(llvm::SMLoc loc);
  llvm::SMLoc getAliasUse() const;
  void rememberBuiltinAlias(TypeAliasDecl *alias);
  TypeAliasDecl *findBuiltinAlias(llvm::StringRef name) const;
  void rememberDescriptorOrigin(const Type *type, const Type *args, const Type *returnType);
  const DescriptorOrigin *findDescriptorOrigin(const Type *type) const;

private:
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, AliasInstance>
      aliasInstances;
  llvm::DenseMap<const Type *, AliasOrigin> aliasOrigins;
  llvm::DenseMap<const TypeAliasDecl *, AliasParams> aliasParams;
  unsigned aliasDepth = 0;
  llvm::SMLoc aliasUse;
  llvm::StringMap<TypeAliasDecl *> builtinAliases;
  llvm::DenseMap<const Type *, DescriptorOrigin> descriptorOrigins;
};

}
