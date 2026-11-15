#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <string>

namespace tz {

class CStringLowerer {
public:
  explicit CStringLowerer(Lowering &owner);

  llvm::Value *emitLiteral(const std::u16string &text);
  llvm::Value *emitFromString(llvm::Value *text);
  llvm::Value *emitToString(llvm::Value *cstring);
  llvm::Value *emitToStringCall(CallExpr *call);
  llvm::Value *emitEqual(llvm::Value *first, llvm::Value *second);
  llvm::Value *emitCopy(llvm::Value *pointer);
  llvm::Value *textPointer(llvm::Value *cstring);

private:
  llvm::FunctionCallee runtimeFunction(llvm::StringRef name, llvm::Type *result,
                                       llvm::ArrayRef<llvm::Type *> params);

  Lowering &lowering;
  llvm::StringMap<llvm::Constant *> literals;
};

}
