#pragma once

#include "compiler/program/symbols/completer/completer.h"

namespace tz {

class SemanticState;

class ReturnCompleter : public Completer {
public:
  explicit ReturnCompleter(SemanticState &owner);

  const Type *completeReturnType(FuncSymbol *fn) override;

private:
  SemanticState &state;
};

}
