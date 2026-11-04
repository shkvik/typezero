#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReflectLowerer {
public:
  explicit ReflectLowerer(Lowering &lowering);

  llvm::Value *emitReflectNew(NewExpr *expr);
  llvm::Value *emitReflectCall(CallExpr *call);
  llvm::StructType *reflectMethodTy();
  llvm::Function *constructFunction(const ClassDecl *cls);
  llvm::Function *fillFunction(const ClassDecl *cls);
  void finishReflection(SourceFile &file, llvm::Instruction *init);
  void useErrors(const Type *errorType);

private:
  Lowering &lowering_;
  llvm::DenseMap<const llvm::Constant *, llvm::GlobalVariable *> reflectBoxes_;
  llvm::MapVector<const ClassDecl *, llvm::Function *> constructFunctions_;
  void noteFillErrors(const Type *errorType);

  llvm::MapVector<const ClassDecl *, llvm::Function *> fillFunctions_;
  const Type *fillTypeError_ = nullptr;
  const Type *fillUnknownError_ = nullptr;
  const Type *fillClassError_ = nullptr;
  bool usesFillAny_ = false;
  bool usesApply_ = false;
  llvm::StructType *reflectMethodTy_ = nullptr;
};

}
