#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The descriptions of classes for reflection.
class ClassInfoLowerer {
public:
  explicit ClassInfoLowerer(CodeGen &codegen);

  // The variants of the method `name` an object of `cls` has, as reflection
  // gives them: its own and those up its chain none of them overrides.
  std::vector<const FunctionDecl *> reflectVariants(const ClassDecl *cls, llvm::StringRef name);
  llvm::StructType *classInfoType();
  llvm::Constant *classInfoGlobal(const ClassDecl *cls);
  void emitConstructThunk(const ClassDecl *cls, llvm::Function *fn);
  // A method as a function value of `type`, `(this: unknown, ...params) =>
  // R`, which checks its `this`: the info of the value.
  llvm::Constant *reflectAdapterInfo(const FunctionDecl *fn, const Type *&type);

private:
  CodeGen &codegen_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> reflectAdapters_;
  llvm::StructType *classInfoTy_ = nullptr;
};

} // namespace tz
