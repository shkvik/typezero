#include "compiler/program/symbols/func_symbol/func_symbol.h"

namespace tz {

bool FuncSymbol::isGeneric() const {
  return !this->typeParams.empty();
}

bool FuncSymbol::hasReturnType() const {
  return this->ret != nullptr;
}

const Type *FuncSymbol::getReturnType() {
  if (this->ret != nullptr || this->completer == nullptr) {
    return this->ret;
  }

  return this->completer->completeReturnType(this);
}

void FuncSymbol::setCompleter(Completer *engine) {
  this->completer = engine;
}

void FuncSymbol::setReturnType(const Type *type) {
  this->ret = type;
}

const Type *FuncSymbol::getKnownReturnType() const {
  return this->ret;
}

}
