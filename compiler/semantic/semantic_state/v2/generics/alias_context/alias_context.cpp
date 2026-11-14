#include "compiler/semantic/semantic_state/v2/generics/alias_context/alias_context.h"

namespace tz {

AliasInstance *AliasContext::recordAliasInstance(const TypeAliasDecl *alias,
                                                 std::vector<const Type *> args) {
  return &this->aliasInstances[{alias, std::move(args)}];
}

void AliasContext::rememberAliasOrigin(const Type *type, TypeAliasDecl *alias,
                                       std::vector<const Type *> args) {
  this->aliasOrigins[type] = {alias, std::move(args)};
}

const AliasOrigin *AliasContext::findAliasOrigin(const Type *type) const {
  auto found = this->aliasOrigins.find(type);
  if (found == this->aliasOrigins.end()) {
    return nullptr;
  }

  return &found->second;
}

void AliasContext::rememberAliasParams(const TypeAliasDecl *alias, AliasParams params) {
  this->aliasParams.try_emplace(alias, std::move(params));
}

std::optional<AliasParams> AliasContext::findAliasParams(const TypeAliasDecl *alias) const {
  auto found = this->aliasParams.find(alias);
  if (found == this->aliasParams.end()) {
    return std::nullopt;
  }

  return found->second;
}

void AliasContext::enterAlias() {
  this->aliasDepth += 1;
}

void AliasContext::leaveAlias() {
  this->aliasDepth -= 1;
}

unsigned AliasContext::getAliasDepth() const {
  return this->aliasDepth;
}

void AliasContext::rememberAliasUse(llvm::SMLoc loc) {
  this->aliasUse = loc;
}

llvm::SMLoc AliasContext::getAliasUse() const {
  return this->aliasUse;
}

}
