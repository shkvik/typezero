#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class MemoryDebug {
public:
  explicit MemoryDebug(Lowering &lowering);

  void checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                  const Type *fieldType);
  void finishGroupDebug(llvm::Instruction *init);
  llvm::Function *traceFunction(const Type *type, llvm::StructType *layout,
                                llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                llvm::StringRef name);

private:
  Lowering &lowering_;
};

}
