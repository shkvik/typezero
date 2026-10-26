#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ClassDescriptors {
public:
  explicit ClassDescriptors(Lowering &lowering);

  void declareMethods(ClassDecl *cls);
  void emitMethods(ClassDecl *cls);
  llvm::Function *methodCode(const FunctionDecl *fn);
  llvm::Constant *classDescriptor(const ClassDecl *cls);
  llvm::Constant *dropDescriptor(llvm::Function *drop);

private:
  Lowering &lowering_;
};

}
