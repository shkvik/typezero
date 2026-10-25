#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The checks of the debug mode of memory.
class MemoryDebug {
public:
  explicit MemoryDebug(CodeGen &codegen);

  // In the drop of an object of `type`, the value of its field `name`, which
  // goes now: an owned object (layer 2) must have no other reference.
  void checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                  const Type *fieldType);
  // The functions that give the walk the references in the members, by
  // layout, handed to the runtime after main's init.
  void finishGroupDebug(llvm::Instruction *init);
  llvm::Function *traceFunction(const Type *type, llvm::StructType *layout,
                                llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                llvm::StringRef name);

private:
  CodeGen &codegen_;
};

} // namespace tz
