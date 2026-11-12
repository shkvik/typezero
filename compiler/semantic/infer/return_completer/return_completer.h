#pragma once

#include "compiler/program/symbols/completer/completer.h"

namespace tz {

class SemanticParts;
class ReturnInferrer;

class SemanticState;

class ReturnCompleter : public Completer {
public:
  ReturnCompleter(SemanticState &owner, SemanticParts &all);

  const Type *completeReturnType(FuncSymbol *function) override;

private:
  SemanticState &state;
  ReturnInferrer *returnInferrer = nullptr;
};

}
