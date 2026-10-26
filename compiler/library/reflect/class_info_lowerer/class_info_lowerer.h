#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ClassInfoLowerer {
public:
  explicit ClassInfoLowerer(Lowering &lowering);

  std::vector<const FunctionDecl *> reflectVariants(const ClassDecl *cls, llvm::StringRef name);
  llvm::StructType *classInfoType();
  llvm::Constant *classInfoGlobal(const ClassDecl *cls);
  void emitConstructThunk(const ClassDecl *cls, llvm::Function *fn);
  llvm::Constant *reflectAdapterInfo(const FunctionDecl *fn, const Type *&type);

private:
  Lowering &lowering_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> reflectAdapters_;
  llvm::StructType *classInfoTy_ = nullptr;
};

}
