#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Function values and the calls through them.
class ClosureLowerer {
public:
  explicit ClosureLowerer(CodeGen &codegen);

  // The type of a variable, also of a function's while Sema left it unset.
  static const Type *typeOf(const VarSymbol *var);
  static const Type *functionTypeOf(const FuncSymbol *fn);
  // (ptr env, params...) -> ret: how a function value of `fnType` is called.
  llvm::FunctionType *closureFnType(const Type *fnType);
  llvm::Function *closureCode(FunctionDecl *fn);
  // The info of a function value: { ptr code, string name }.
  llvm::Constant *functionInfo(FunctionDecl *fn);
  llvm::Value *makeFunctionValue(llvm::Value *info, llvm::Value *env);
  llvm::Value *emitFunctionExpr(FunctionExpr *fe);
  llvm::Value *emitFunctionAdapter(CastExpr *cast);
  // The environment for a direct call to a closure.
  llvm::Value *closureEnv(CallExpr *call);
  llvm::Value *emitValueCall(CallExpr *call);
  // Calls a borrowed function value with owned arguments; the parameters
  // after them get undefined.
  llvm::Value *callFunctionValue(llvm::Value *fn, const Type *fnType,
                                 llvm::ArrayRef<llvm::Value *> args);
  void emitClosure(FunctionDecl *fn);
  void emitPendingClosures();

private:
  CodeGen &codegen_;
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> closureCode_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> infos_;
  std::map<std::pair<const FunctionDecl *, const Type *>, llvm::Constant *> adapters_;
};

} // namespace tz
