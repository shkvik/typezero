#include "compiler/pipeline/check_program/check_state/callback_facts/callback_facts.h"

namespace tz {

void CallbackFacts::rememberByteCallback(const FunctionDecl *fn, const ByteCallback &callback) {
  this->byteCallbacks[fn] = callback;
}

const ByteCallback *CallbackFacts::findByteCallback(const FunctionDecl *fn) const {
  auto found = this->byteCallbacks.find(fn);
  if (found == this->byteCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

void CallbackFacts::rememberIndexCallback(const FunctionDecl *fn, unsigned param) {
  this->indexCallbacks[fn] = param;
}

std::optional<unsigned> CallbackFacts::findIndexParam(const FunctionDecl *fn) const {
  auto found = this->indexCallbacks.find(fn);
  if (found == this->indexCallbacks.end()) {
    return std::nullopt;
  }

  return found->second;
}

void CallbackFacts::rememberElementCallback(const FunctionDecl *fn,
                                            const ElementCallback &callback) {
  this->elementCallbacks[fn] = callback;
}

const ElementCallback *CallbackFacts::findElementCallback(const FunctionDecl *fn) const {
  auto found = this->elementCallbacks.find(fn);
  if (found == this->elementCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

}
