#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PublishLowerer {
public:
  explicit PublishLowerer(Lowering &owner);

  void pinValue(llvm::Value *value, const Type *type);
  void endValue(llvm::Value *value, const Type *type);
  void finishPublishTable(llvm::Instruction *init);
  llvm::Function *publishFunction(const Type *type, llvm::StructType *layout,
                                  llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                  llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                  llvm::StringRef name);

private:
  static llvm::Value *spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name);

  Lowering &lowering;
};

}
