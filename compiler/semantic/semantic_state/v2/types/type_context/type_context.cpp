#include "compiler/semantic/semantic_state/v2/types/type_context/type_context.h"

namespace tz {

void TypeContext::enterExtendsClause() {
  this->extendsClauseDepth += 1;
}

void TypeContext::leaveExtendsClause() {
  this->extendsClauseDepth -= 1;
}

bool TypeContext::isInsideExtendsClause() const {
  return this->extendsClauseDepth > 0;
}

void TypeContext::enterListContext(bool isList) {
  this->listContexts.push_back(isList);
}

void TypeContext::leaveListContext() {
  this->listContexts.pop_back();
}

bool TypeContext::isInsideListContext() const {
  return !this->listContexts.empty() && this->listContexts.back();
}

void TypeContext::allowTailCall() {
  this->tailCallAllowed = true;
}

void TypeContext::forbidTailCall() {
  this->tailCallAllowed = false;
}

bool TypeContext::isTailCallAllowed() const {
  return this->tailCallAllowed;
}

void TypeContext::rememberTailCall(TypeAliasDecl *alias, std::vector<const Type *> args) {
  this->tailCall = TailCall{alias, std::move(args)};
}

bool TypeContext::hasTailCall() const {
  return this->tailCall.has_value();
}

TailCall TypeContext::takeTailCall() {
  TailCall call = std::move(*this->tailCall);
  this->tailCall.reset();

  return call;
}

const ConditionalInfo *TypeContext::addConditional(ConditionalInfo info) {
  const TypeRef *ref = info.ref;
  ConditionalInfo &stored = this->conditionals[ref];
  stored = std::move(info);
  this->conditionalTags[stored.tag] = &stored;

  return &stored;
}

const ConditionalInfo *TypeContext::findConditional(const TypeRef *ref) const {
  auto found = this->conditionals.find(ref);
  if (found == this->conditionals.end()) {
    return nullptr;
  }

  return &found->second;
}

const ConditionalInfo *TypeContext::findConditionalByTag(const Type *tag) const {
  return this->conditionalTags.lookup(tag);
}

void TypeContext::rememberTemplateLoc(llvm::SMLoc loc) {
  this->templateLoc = loc;
}

void TypeContext::forgetTemplateLoc() {
  this->templateLoc = llvm::SMLoc();
}

llvm::SMLoc TypeContext::getTemplateLoc() const {
  return this->templateLoc;
}

}
