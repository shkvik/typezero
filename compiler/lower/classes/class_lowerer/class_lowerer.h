#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Instances: new, constructors, methods, instanceof and static fields.
class ClassLowerer {
public:
  explicit ClassLowerer(CodeGen &codegen);

  // A new instance whose fields are empty (zero).
  llvm::Value *allocInstance(const ClassDecl *cls);
  llvm::Value *allocInstance(const ClassDecl *cls, bool member);
  llvm::Value *emitNew(NewExpr *expr);
  llvm::Value *emitMethodCall(CallExpr *call);
  llvm::Value *emitBoundMethod(MemberExpr *member);
  llvm::Value *emitInstanceOf(BinaryExpr *bin);
  llvm::Value *emitMethodLiteral(ObjectLitExpr *lit);
  void emitStaticFields(ClassDecl *cls);
  // `new C(args)` and the part of `super(args)` that runs the class
  // extended: the initializers of the fields and the constructor `ctor` (of
  // `cls` or of a class up its chain), with `args` owned.
  void emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                     llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *emitSuperCall(CallExpr *call);
  // Whether constructing an object of `cls` with `ctor` may throw.
  bool constructionThrows(const ClassDecl *cls, const FuncSymbol *ctor) const;

private:
  CodeGen &codegen_;
};

} // namespace tz
