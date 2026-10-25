#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Classes with decorators: stubs and adapters of methods, metadata.
class DecoratorLowerer {
public:
  explicit DecoratorLowerer(CodeGen &codegen);

  // Decorators (codegen_decorate.cpp). A class declaration sets the static
  // fields, gives the methods that decorators may replace (FunctionDecl::impl)
  // their adapters, and runs the decorators.
  void emitClassDeclaration(ClassDecl *cls);
  // Such a method: its body as `.original`, and the stub that calls what its
  // global holds.
  void emitDecoratedMethod(FunctionDecl *fn);
  llvm::Function *originalCode(FunctionDecl *fn);
  // The info of the function value that runs the body: the first `value` of
  // the descriptor.
  llvm::Constant *implAdapterInfo(FunctionDecl *fn);
  // Metadata: Reflect.defineMetadata and the others; the constant whose
  // address is an object key; and at the end, if the program uses
  // metadata, the objects of its classes for the runtime, given to it right
  // after `init` in main.
  llvm::Value *emitMetaCall(CallExpr *call);
  llvm::Constant *metaKeyConstant(const void *key);
  void finishMetadata(SourceFile &file, llvm::Instruction *init);
  // Classes as values and reflection (codegen_reflect.cpp, docs/decorators.md
  // sections 5 and 6). A class expression is its class's object, after its
  // static fields get their values. The classes reflection reaches get a
  // description (TzClassInfo in runtime/core/tzrt/tzrt.h) that the object of
  // the class points to: the function that makes an object from arguments as
  // unknowns, the one that makes it from a Json, and its methods as functions
  // that take their `this`. A function value in an unknown is called through
  // the invoker of its type.
  llvm::Value *emitClassExpr(ClassExpr *expr);

private:
  CodeGen &codegen_;
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> originals_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> adapterInfos_;
  llvm::DenseMap<const void *, llvm::Constant *> metaKeys_;
  bool usesMetadata_ = false;
};

} // namespace tz
