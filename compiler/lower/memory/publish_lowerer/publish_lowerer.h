#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PublishLowerer {
public:
  explicit PublishLowerer(Lowering &lowering);

  void pinValue(llvm::Value *value, const Type *type);
  void endValue(llvm::Value *value, const Type *type);
  void finishPublishTable(llvm::Instruction *init);
  llvm::Function *publishFunction(const Type *type, llvm::StructType *layout,
                                  llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                  llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                  llvm::StringRef name);

private:
  Lowering &lowering_;
};

}
