#pragma once

#include "compiler/program/ast/builtin/builtin.h"

namespace llvm {
class Value;
}

namespace tz {

class Lowering;
class CallExpr;

class LibraryLowerer {
public:
  virtual ~LibraryLowerer() = default;

  virtual bool lowers(Builtin builtin) const = 0;
  virtual llvm::Value *lowerCall(Lowering &lowering, CallExpr *call) const = 0;
};

}
