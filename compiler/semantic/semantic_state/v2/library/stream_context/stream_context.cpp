#include "compiler/semantic/semantic_state/v2/library/stream_context/stream_context.h"

namespace tz {

void StreamContext::rememberStreamView(const FieldPath &path, llvm::SMLoc from, const Type *view) {
  this->streamViews.push_back({path, from, view});
}

bool StreamContext::hasStreamViews() const {
  return !this->streamViews.empty();
}

const Type *StreamContext::findStreamView(const FieldPath &path, llvm::SMLoc loc) const {
  for (auto it = this->streamViews.rbegin(); it != this->streamViews.rend(); ++it) {
    if (it->path == path && it->from.getPointer() < loc.getPointer()) {
      return it->type;
    }
  }

  return nullptr;
}

void StreamContext::rememberProcessStdin(VarSymbol *variable) {
  this->processStdin = variable;
}

VarSymbol *StreamContext::getProcessStdin() const {
  return this->processStdin;
}

void StreamContext::rememberHookParams(const FunctionDecl *functionDecl,
                                       std::vector<const Type *> params) {
  this->hookParams[functionDecl] = std::move(params);
}

std::optional<std::vector<const Type *>>
StreamContext::findHookParams(const FunctionDecl *functionDecl) const {
  auto found = this->hookParams.find(functionDecl);
  if (found == this->hookParams.end()) {
    return std::nullopt;
  }

  return found->second;
}

}
