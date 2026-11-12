#include "compiler/semantic/semantic_state/stream_store/stream_store.h"

namespace tz {

void StreamStore::rememberStreamView(const FieldPath &path, llvm::SMLoc from, const Type *view) {
  this->streamViews.push_back({path, from, view});
}

bool StreamStore::hasStreamViews() const {
  return !this->streamViews.empty();
}

const Type *StreamStore::findStreamView(const FieldPath &path, llvm::SMLoc loc) const {
  for (auto it = this->streamViews.rbegin(); it != this->streamViews.rend(); ++it) {
    if (it->path == path && it->from.getPointer() < loc.getPointer()) {
      return it->type;
    }
  }

  return nullptr;
}

void StreamStore::rememberProcessStdin(VarSymbol *variable) {
  this->processStdin = variable;
}

VarSymbol *StreamStore::getProcessStdin() const {
  return this->processStdin;
}

}
