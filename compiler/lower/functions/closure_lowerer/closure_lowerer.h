#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ClosureLowerer {
public:
  explicit ClosureLowerer(Lowering &lowering);

  static const Type *typeOf(const VarSymbol *var);
  static const Type *functionTypeOf(const FuncSymbol *fn);
  llvm::FunctionType *closureFnType(const Type *fnType);
  llvm::Function *closureCode(FunctionDecl *fn);
  llvm::Constant *functionInfo(FunctionDecl *fn);
  llvm::Value *makeFunctionValue(llvm::Value *info, llvm::Value *env);
  llvm::Value *emitFunctionExpr(FunctionExpr *fe);
  llvm::Value *emitFunctionAdapter(CastExpr *cast);
  llvm::Value *closureEnv(CallExpr *call);
  llvm::Value *emitValueCall(CallExpr *call);
  llvm::Value *callFunctionValue(llvm::Value *fn, const Type *fnType,
                                 llvm::ArrayRef<llvm::Value *> args);
  void emitClosure(FunctionDecl *fn);
  void emitPendingClosures();

private:
  Lowering &lowering_;
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> closureCode_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> infos_;
  std::map<std::pair<const FunctionDecl *, const Type *>, llvm::Constant *> adapters_;
};

}
