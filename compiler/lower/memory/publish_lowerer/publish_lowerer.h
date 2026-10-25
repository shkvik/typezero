#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Values published to other threads, and the values pinned for them.
class PublishLowerer {
public:
  explicit PublishLowerer(Lowering &lowering);

  void pinValue(llvm::Value *value, const Type *type);
  void endValue(llvm::Value *value, const Type *type);
  // The parallel runtime (runtime/core/publish/publish.c): for each descriptor
  // of an object the program makes, the function that publishes what the object
  // holds, handed over at the start of main.
  void finishPublishTable(llvm::Instruction *init);
  llvm::Function *publishFunction(const Type *type, llvm::StructType *layout,
                                  llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                  llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                  llvm::StringRef name);

private:
  Lowering &lowering_;
};

} // namespace tz
