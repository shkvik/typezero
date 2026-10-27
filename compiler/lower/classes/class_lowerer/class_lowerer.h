#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ClassLowerer {
public:
  explicit ClassLowerer(Lowering &lowering);

  llvm::Value *allocInstance(const ClassDecl *cls);
  llvm::Value *allocInstance(const ClassDecl *cls, bool member);
  llvm::Value *emitNew(NewExpr *expr);
  llvm::Value *emitMethodCall(CallExpr *call);
  llvm::Value *emitBoundMethod(MemberExpr *member);
  llvm::Value *emitInstanceOf(BinaryExpr *bin);
  llvm::Value *emitMethodLiteral(ObjectLitExpr *lit);
  void emitStaticFields(ClassDecl *cls);
  void emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                     llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *emitSuperCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
