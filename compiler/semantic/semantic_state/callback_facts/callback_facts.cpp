#include "compiler/semantic/semantic_state/callback_facts/callback_facts.h"

namespace tz {

void CallbackFacts::rememberByteCallback(const FunctionDecl *functionDecl,
                                         const ByteCallback &callback) {
  this->byteCallbacks[functionDecl] = callback;
}

const ByteCallback *CallbackFacts::findByteCallback(const FunctionDecl *functionDecl) const {
  auto found = this->byteCallbacks.find(functionDecl);
  if (found == this->byteCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

void CallbackFacts::rememberIndexCallback(const FunctionDecl *functionDecl, unsigned param) {
  this->indexCallbacks[functionDecl] = param;
}

std::optional<unsigned> CallbackFacts::findIndexParam(const FunctionDecl *functionDecl) const {
  auto found = this->indexCallbacks.find(functionDecl);
  if (found == this->indexCallbacks.end()) {
    return std::nullopt;
  }

  return found->second;
}

void CallbackFacts::rememberElementCallback(const FunctionDecl *functionDecl,
                                            const ElementCallback &callback) {
  this->elementCallbacks[functionDecl] = callback;
}

const ElementCallback *CallbackFacts::findElementCallback(const FunctionDecl *functionDecl) const {
  auto found = this->elementCallbacks.find(functionDecl);
  if (found == this->elementCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

}
