#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReflectLowerer {
public:
  explicit ReflectLowerer(Lowering &owner);

  llvm::Value *emitReflectNew(NewExpr *expr);
  llvm::Value *emitReflectCall(CallExpr *call);
  llvm::StructType *reflectMethodTy();
  llvm::Function *constructFunction(const ClassDecl *cls);
  llvm::Function *fillFunction(const ClassDecl *cls);
  void finishReflection(SourceFile &file, llvm::Instruction *init);
  void useErrors(const Type *errorType);

private:
  static std::string className(const ClassDecl *cls);
  static std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls);

  Lowering &lowering;
  llvm::DenseMap<const llvm::Constant *, llvm::GlobalVariable *> reflectBoxes;
  llvm::MapVector<const ClassDecl *, llvm::Function *> constructFunctions;
  void noteFillErrors(const Type *errorType);

  llvm::MapVector<const ClassDecl *, llvm::Function *> fillFunctions;
  const Type *fillTypeError = nullptr;
  const Type *fillUnknownError = nullptr;
  const Type *fillClassError = nullptr;
  bool usesFillAny = false;
  bool usesApply = false;
  llvm::StructType *reflectMethodType = nullptr;
};

}
