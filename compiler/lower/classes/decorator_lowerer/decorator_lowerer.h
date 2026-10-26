#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class DecoratorLowerer {
public:
  explicit DecoratorLowerer(Lowering &lowering);

  void emitClassDeclaration(ClassDecl *cls);
  void emitDecoratedMethod(FunctionDecl *fn);
  llvm::Function *originalCode(FunctionDecl *fn);
  llvm::Constant *implAdapterInfo(FunctionDecl *fn);
  llvm::Value *emitMetaCall(CallExpr *call);
  llvm::Constant *metaKeyConstant(const void *key);
  void finishMetadata(SourceFile &file, llvm::Instruction *init);
  llvm::Value *emitClassExpr(ClassExpr *expr);

private:
  Lowering &lowering_;
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> originals_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> adapterInfos_;
  llvm::DenseMap<const void *, llvm::Constant *> metaKeys_;
  bool usesMetadata_ = false;
};

}
