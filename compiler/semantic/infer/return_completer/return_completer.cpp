#include "compiler/semantic/infer/return_completer/return_completer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

ReturnCompleter::ReturnCompleter(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->returnInferrer = &all.returnInferrer;
}

const Type *ReturnCompleter::completeReturnType(FuncSymbol *fn) {
  return this->returnInferrer->returnType(fn);
}

}
