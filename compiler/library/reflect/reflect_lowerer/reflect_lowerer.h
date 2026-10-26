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
  llvm::Function *classFromJsonFunction(const ClassDecl *cls);
  void emitClassFromJson(const ClassDecl *cls, llvm::Function *fn);
  void finishReflection(SourceFile &file, llvm::Instruction *init);
  void useErrors(const Type *errorType);

private:
  Lowering &lowering_;
  llvm::DenseMap<const ClassDecl *, llvm::Function *> classConverters_;
  std::vector<const ClassDecl *> pendingConverters_;
  llvm::DenseMap<const llvm::Constant *, llvm::GlobalVariable *> reflectBoxes_;
  llvm::MapVector<const ClassDecl *, llvm::Function *> constructFunctions_;
  bool usesApply_ = false;
  llvm::StructType *reflectMethodTy_ = nullptr;
};

}
