#include "compiler/semantic/semantic_state/v2/flow/callback_context/callback_context.h"

namespace tz {

void CallbackContext::rememberByteCallback(const FunctionDecl *functionDecl,
                                           const ByteCallback &callback) {
  this->byteCallbacks[functionDecl] = callback;
}

const ByteCallback *CallbackContext::findByteCallback(const FunctionDecl *functionDecl) const {
  auto found = this->byteCallbacks.find(functionDecl);
  if (found == this->byteCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

void CallbackContext::rememberIndexCallback(const FunctionDecl *functionDecl, unsigned param) {
  this->indexCallbacks[functionDecl] = param;
}

std::optional<unsigned> CallbackContext::findIndexParam(const FunctionDecl *functionDecl) const {
  auto found = this->indexCallbacks.find(functionDecl);
  if (found == this->indexCallbacks.end()) {
    return std::nullopt;
  }

  return found->second;
}

void CallbackContext::rememberElementCallback(const FunctionDecl *functionDecl,
                                              const ElementCallback &callback) {
  this->elementCallbacks[functionDecl] = callback;
}

const ElementCallback *
CallbackContext::findElementCallback(const FunctionDecl *functionDecl) const {
  auto found = this->elementCallbacks.find(functionDecl);
  if (found == this->elementCallbacks.end()) {
    return nullptr;
  }

  return &found->second;
}

}
