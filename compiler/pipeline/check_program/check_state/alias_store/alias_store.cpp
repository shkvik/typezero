#include "compiler/pipeline/check_program/check_state/alias_store/alias_store.h"

namespace tz {

AliasInstance *AliasStore::recordAliasInstance(const TypeAliasDecl *alias,
                                               std::vector<const Type *> args) {
  return &this->aliasInstances[{alias, std::move(args)}];
}

void AliasStore::rememberAliasOrigin(const Type *type, TypeAliasDecl *alias,
                                     std::vector<const Type *> args) {
  this->aliasOrigins[type] = {alias, std::move(args)};
}

const AliasOrigin *AliasStore::findAliasOrigin(const Type *type) const {
  auto found = this->aliasOrigins.find(type);
  if (found == this->aliasOrigins.end()) {
    return nullptr;
  }

  return &found->second;
}

void AliasStore::rememberAliasParams(const TypeAliasDecl *alias, AliasParams params) {
  this->aliasParams.try_emplace(alias, std::move(params));
}

std::optional<AliasParams> AliasStore::findAliasParams(const TypeAliasDecl *alias) const {
  auto found = this->aliasParams.find(alias);
  if (found == this->aliasParams.end()) {
    return std::nullopt;
  }

  return found->second;
}

void AliasStore::enterAlias() {
  this->aliasDepth += 1;
}

void AliasStore::leaveAlias() {
  this->aliasDepth -= 1;
}

unsigned AliasStore::getAliasDepth() const {
  return this->aliasDepth;
}

void AliasStore::rememberAliasUse(llvm::SMLoc loc) {
  this->aliasUse = loc;
}

llvm::SMLoc AliasStore::getAliasUse() const {
  return this->aliasUse;
}

void AliasStore::rememberBuiltinAlias(TypeAliasDecl *alias) {
  this->builtinAliases[alias->name] = alias;
}

TypeAliasDecl *AliasStore::findBuiltinAlias(llvm::StringRef name) const {
  return this->builtinAliases.lookup(name);
}

void AliasStore::rememberDescriptorOrigin(const Type *type, const Type *args, const Type *ret) {
  this->descriptorOrigins.try_emplace(type, DescriptorOrigin{args, ret});
}

const DescriptorOrigin *AliasStore::findDescriptorOrigin(const Type *type) const {
  auto found = this->descriptorOrigins.find(type);
  if (found == this->descriptorOrigins.end()) {
    return nullptr;
  }

  return &found->second;
}

}
