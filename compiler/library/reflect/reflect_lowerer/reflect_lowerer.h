#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Reflect.get, apply and construct, and fromJson of classes.
class ReflectLowerer {
public:
  explicit ReflectLowerer(Lowering &lowering);

  llvm::Value *emitReflectNew(NewExpr *expr);
  llvm::Value *emitReflectCall(CallExpr *call);
  llvm::StructType *reflectMethodTy();
  // `ptr (ptr args, i32 count)`: an object of the class from arguments as
  // unknowns, checked against its constructors; its body is emitted at the
  // end. The description has it, and so does Reflect.construct of the class
  // known when compiling.
  llvm::Function *constructFunction(const ClassDecl *cls);
  // `ptr (ptr json, ptr path)`: an object of the class from a Json, through
  // its constructor; its body is emitted at the end.
  llvm::Function *classFromJsonFunction(const ClassDecl *cls);
  void emitClassFromJson(const ClassDecl *cls, llvm::Function *fn);
  // At the end: the descriptions, and what main gives the runtime first.
  void finishReflection(SourceFile &file, llvm::Instruction *init);

private:
  Lowering &lowering_;
  llvm::DenseMap<const ClassDecl *, llvm::Function *> classConverters_;
  std::vector<const ClassDecl *> pendingConverters_;
  // Of Builtin::ReflectMethod: the box of each method's function, by its
  // info.
  llvm::DenseMap<const llvm::Constant *, llvm::GlobalVariable *> reflectBoxes_;
  llvm::MapVector<const ClassDecl *, llvm::Function *> constructFunctions_;
  bool usesApply_ = false;
  llvm::StructType *reflectMethodTy_ = nullptr;
};

} // namespace tz
