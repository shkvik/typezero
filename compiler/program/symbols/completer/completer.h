#pragma once

namespace tz {

class Type;
class FuncSymbol;

class Completer {
public:
  virtual ~Completer() = default;

  virtual const Type *completeReturnType(FuncSymbol *fn) = 0;
};

}
