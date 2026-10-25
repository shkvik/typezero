#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Number, parseInt and the like, String.fromCharCode and toFixed.
class NumberLowerer {
public:
  explicit NumberLowerer(CodeGen &codegen);

  llvm::Value *emitNumberBuiltin(CallExpr *call);
  // Number(s) of a string; `str` is borrowed.
  llvm::Value *emitStringToNumber(llvm::Value *str);
  // A double that must be a whole number fitting in `to` (an int or a long),
  // or the program stops.
  llvm::Value *emitCheckedInteger(llvm::Value *value, const Type *to);

private:
  CodeGen &codegen_;
};

} // namespace tz
