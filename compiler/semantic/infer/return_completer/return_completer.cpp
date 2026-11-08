#include "compiler/semantic/infer/return_completer/return_completer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"

namespace tz {

ReturnCompleter::ReturnCompleter(SemanticState &owner)
    : state(owner) {}

const Type *ReturnCompleter::completeReturnType(FuncSymbol *fn) {
  return ReturnInferrer(this->state).returnType(fn);
}

}
